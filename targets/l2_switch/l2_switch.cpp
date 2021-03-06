/* Copyright 2013-present Barefoot Networks, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *   http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * Antonin Bas (antonin@barefootnetworks.com)
 *
 */

#include <iostream>
#include <memory>
#include <thread>
#include <fstream>
#include <string>
#include <chrono>

#include <unistd.h>

#include "bm_sim/queue.h"
#include "bm_sim/packet.h"
#include "bm_sim/parser.h"
#include "bm_sim/tables.h"
#include "bm_sim/switch.h"
#include "bm_sim/event_logger.h"
#include "bm_sim/simple_pre.h"

#include "l2_switch.h"
#include "primitives.h"
#include "simplelog.h"

#include "bm_runtime/bm_runtime.h"

// #define PCAP_DUMP

class SimpleSwitch : public Switch {
public:
  SimpleSwitch()
    : input_buffer(1024), output_buffer(128), pre(new McSimplePre()) {
    add_component<McSimplePre>(pre);
  }

  int receive(int port_num, const char *buffer, int len) {
    static int pkt_id = 0;

    Packet *packet =
      new Packet(port_num, pkt_id++, 0, len, PacketBuffer(2048, buffer, len));

    ELOGGER->packet_in(*packet);

    input_buffer.push_front(std::unique_ptr<Packet>(packet));
    return 0;
  }

  void start_and_return() {
    std::thread t1(&SimpleSwitch::pipeline_thread, this);
    t1.detach();
    std::thread t2(&SimpleSwitch::transmit_thread, this);
    t2.detach();
  }

private:
  void pipeline_thread();
  void transmit_thread();

private:
  Queue<std::unique_ptr<Packet> > input_buffer;
  Queue<std::unique_ptr<Packet> > output_buffer;
  std::shared_ptr<McSimplePre> pre;
};

void SimpleSwitch::transmit_thread() {
  while(1) {
    std::unique_ptr<Packet> packet;
    output_buffer.pop_back(&packet);
    ELOGGER->packet_out(*packet);
    SIMPLELOG << "transmitting packet " << packet->get_packet_id() << std::endl;
    transmit_fn(packet->get_egress_port(), packet->data(), packet->get_data_size());
  }
}

void SimpleSwitch::pipeline_thread() {
  Pipeline *ingress_mau = this->get_pipeline("ingress");
  Pipeline *egress_mau = this->get_pipeline("egress");
  Parser *parser = this->get_parser("parser");
  Deparser *deparser = this->get_deparser("deparser");
  PHV *phv;

  while(1) {
    std::unique_ptr<Packet> packet;
    input_buffer.pop_back(&packet);
    phv = packet->get_phv();
    SIMPLELOG << "processing packet " << packet->get_packet_id() << std::endl;

    int ingress_port = packet->get_ingress_port();
    phv->get_field("standard_metadata.ingress_port").set(ingress_port);
    ingress_port = phv->get_field("standard_metadata.ingress_port").get_int();
    std::cout << ingress_port << std::endl;
    
    parser->parse(packet.get());
    ingress_mau->apply(packet.get());

    int egress_port = phv->get_field("standard_metadata.egress_port").get_int();
    SIMPLELOG << "egress port is " << egress_port << std::endl;

    int learn_id = phv->get_field("intrinsic_metadata.learn_id").get_int();
    SIMPLELOG << "learn id is " << learn_id << std::endl;

    unsigned int mgid = phv->get_field("intrinsic_metadata.mgid").get_uint();
    SIMPLELOG << "mgid is " << mgid << std::endl;

    if(learn_id > 0) {
      get_learn_engine()->learn(learn_id, *packet.get());
      phv->get_field("intrinsic_metadata.learn_id").set(0);
    }

    if(egress_port == 511 && mgid == 0) {
      SIMPLELOG << "dropping packet\n";
      continue;
    }

    if(mgid != 0) {
      assert(mgid == 1);
      phv->get_field("intrinsic_metadata.mgid").set(0);
      packet_id_t copy_id = 1;
      const auto pre_out = pre->replicate({mgid});
      for(const auto &out : pre_out) {
	egress_port = out.egress_port;
	if(ingress_port == egress_port) continue; // pruning
	SIMPLELOG << "replicating packet out of port " << egress_port
		  << std::endl;
	std::unique_ptr<Packet> packet_copy(new Packet());
	*packet_copy = packet->clone(copy_id++);
	packet_copy->set_egress_port(egress_port);
	egress_mau->apply(packet_copy.get());
	deparser->deparse(packet_copy.get());
	output_buffer.push_front(std::move(packet_copy));
      }
    }
    else {
      packet->set_egress_port(egress_port);
      egress_mau->apply(packet.get());
      deparser->deparse(packet.get());
      output_buffer.push_front(std::move(packet));
    }
  }
}

/* Switch instance */

static SimpleSwitch *simple_switch;


int 
main(int argc, char* argv[])
{
  simple_switch = new SimpleSwitch();
  int status = simple_switch->init_from_command_line_options(argc, argv);
  if(status != 0) std::exit(status);

  int thrift_port = simple_switch->get_runtime_port();
  bm_runtime::start_server(simple_switch, thrift_port);

  simple_switch->start_and_return();

  while(1) std::this_thread::sleep_for(std::chrono::seconds(100));
  
  return 0; 
}
