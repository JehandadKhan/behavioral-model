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
#include "bm_sim/simple_pre_lag.h"

#include "simple_switch.h"
#include "primitives.h"
#include "simplelog.h"

#include "bm_runtime/bm_runtime.h"

using std::chrono::microseconds;
using std::chrono::duration_cast;

class SimpleSwitch : public Switch {
private:
  typedef std::chrono::high_resolution_clock clock;

public:
  SimpleSwitch()
    : Switch(false), // enable_switch = false
      input_buffer(1024), egress_buffer(1024), output_buffer(128),
      pre(new McSimplePreLAG()),
      start(clock::now()) {
    add_component<McSimplePreLAG>(pre);

    add_required_field("standard_metadata", "ingress_port");
    add_required_field("standard_metadata", "packet_length");
    add_required_field("standard_metadata", "instance_type");
    add_required_field("standard_metadata", "egress_spec");
    add_required_field("standard_metadata", "clone_spec");
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
    std::thread t1(&SimpleSwitch::ingress_thread, this);
    t1.detach();
    std::thread t2(&SimpleSwitch::egress_thread, this);
    t2.detach();
    std::thread t3(&SimpleSwitch::transmit_thread, this);
    t3.detach();
  }

private:
  enum PktInstanceType {
    PKT_INSTANCE_TYPE_NORMAL,
    PKT_INSTANCE_TYPE_INGRESS_CLONE,
    PKT_INSTANCE_TYPE_EGRESS_CLONE,
    PKT_INSTANCE_TYPE_COALESCED,
    PKT_INSTANCE_TYPE_INGRESS_RECIRC,
    PKT_INSTANCE_TYPE_REPLICATION,
    PKT_INSTANCE_TYPE_RESUBMIT,
  };

private:
  void ingress_thread();
  void egress_thread();
  void transmit_thread();

private:
  Queue<std::unique_ptr<Packet> > input_buffer;
  Queue<std::unique_ptr<Packet> > egress_buffer;
  Queue<std::unique_ptr<Packet> > output_buffer;
  std::shared_ptr<McSimplePreLAG> pre;
  clock::time_point start;
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

void SimpleSwitch::ingress_thread() {
  Parser *parser = this->get_parser("parser");
  Pipeline *ingress_mau = this->get_pipeline("ingress");
  PHV *phv;

  while(1) {
    std::unique_ptr<Packet> packet;
    input_buffer.pop_back(&packet);
    phv = packet->get_phv();
    // many current P4 programs assume this
    // it is also part of the original P4 spec
    phv->reset_metadata();
    int ingress_port = packet->get_ingress_port();
    SIMPLELOG << "processing packet " << packet->get_packet_id()
	      << " received on port "<< ingress_port << std::endl;

    if(phv->has_field("intrinsic_metadata.ingress_global_timestamp")) {
      phv->get_field("intrinsic_metadata.ingress_global_timestamp")
        .set(duration_cast<microseconds>(clock::now() - start).count());
    }

    // setting standard metadata
    phv->get_field("standard_metadata.ingress_port").set(ingress_port);
    int ingress_length = packet->get_ingress_length();
    phv->get_field("standard_metadata.packet_length").set(ingress_length);
    Field &f_instance_type = phv->get_field("standard_metadata.instance_type");
    f_instance_type.set(PKT_INSTANCE_TYPE_NORMAL);

    parser->parse(packet.get());

    ingress_mau->apply(packet.get());

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    int egress_spec = f_egress_spec.get_int();

    Field &f_clone_spec = phv->get_field("standard_metadata.clone_spec");
    int clone_spec = f_clone_spec.get_int();

    int learn_id = 0;
    unsigned int mgid = 0u;

    if(phv->has_header("intrinsic_metadata")) {
      Field &f_learn_id = phv->get_field("intrinsic_metadata.lf_field_list");
      learn_id = f_learn_id.get_int();

      Field &f_mgid = phv->get_field("intrinsic_metadata.mcast_grp");
      mgid = f_mgid.get_uint();
    }

    packet_id_t copy_id = 1;
    int egress_port;

    // INGRESS CLONING
    if(clone_spec) {
      SIMPLELOG << "cloning packet at ingress" << std::endl;
      f_instance_type.set(PKT_INSTANCE_TYPE_INGRESS_CLONE);
      std::unique_ptr<Packet> packet_copy(new Packet(packet->clone(copy_id++)));
      // TODO: this is not how it works !!!
      packet_copy->set_egress_port(clone_spec);
      egress_buffer.push_front(std::move(packet_copy));
      f_instance_type.set(PKT_INSTANCE_TYPE_NORMAL);
    }
    
    // LEARNING
    if(learn_id > 0) {
      get_learn_engine()->learn(learn_id, *packet.get());
    }

    // MULTICAST
    int instance_type = f_instance_type.get_int();
    if(mgid != 0) {
      SIMPLELOG << "multicast\n";
      Field &f_rid = phv->get_field("intrinsic_metadata.egress_rid");
      const auto pre_out = pre->replicate({mgid});
      for(const auto &out : pre_out) {
	egress_port = out.egress_port;
	// if(ingress_port == egress_port) continue; // pruning
	SIMPLELOG << "replicating packet out of port " << egress_port
		  << std::endl;
	f_rid.set(out.rid);
	f_instance_type.set(PKT_INSTANCE_TYPE_REPLICATION);
	std::unique_ptr<Packet> packet_copy(new Packet(packet->clone(copy_id++)));
	packet_copy->set_egress_port(egress_port);
	egress_buffer.push_front(std::move(packet_copy));
      }
      f_instance_type.set(instance_type);

      // when doing multicast, we discard the original packet
      continue;
    }

    egress_port = egress_spec;
    SIMPLELOG << "egress port is " << egress_port << std::endl;    

    if(egress_port == 511) {  // drop packet
      SIMPLELOG << "dropping packet\n";
      continue;
    }

    packet->set_egress_port(egress_port);
    egress_buffer.push_front(std::move(packet));
  }
}

void SimpleSwitch::egress_thread() {
  Deparser *deparser = this->get_deparser("deparser");
  Pipeline *egress_mau = this->get_pipeline("egress");
  PHV *phv;

  while(1) {
    std::unique_ptr<Packet> packet;
    egress_buffer.pop_back(&packet);
    phv = packet->get_phv();

    int egress_port = packet->get_egress_port();
    phv->get_field("standard_metadata.egress_port").set(egress_port);

    Field &f_egress_spec = phv->get_field("standard_metadata.egress_spec");
    f_egress_spec.set(0);

    egress_mau->apply(packet.get());

    // TODO: should not be done like this in egress pipeline
    int egress_spec = f_egress_spec.get_int();
    if(egress_spec == 511) {  // drop packet
      SIMPLELOG << "dropping packet\n";
      continue;
    }

    deparser->deparse(packet.get());

    // TODO: egress cloning

    output_buffer.push_front(std::move(packet));
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
