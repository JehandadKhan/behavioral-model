#include <iostream>

#include <thrift_endpoint.h>

#include <pd/pd_tables.h>
#include <pd/pd_static.h>
#include <pd/pd.h>

#define DEVICE_THRIFT_PORT 9090

int main() {
  start_server();

  p4_pd_init();
  
  p4_pd_sess_hdl_t sess_hdl;
  p4_pd_client_init(&sess_hdl, 16);
  
  std::cerr << "session handle is " << sess_hdl << std::endl;
  
  p4_pd_dev_target_t dev_tgt = {0, 0xFF};
  p4_pd_entry_hdl_t entry_hdl;

  /* P4 dependent initialization */
  p4_pd_test_init(sess_hdl, NULL);
  p4_pd_test_assign_device(sess_hdl, dev_tgt.device_id, DEVICE_THRIFT_PORT);
  
  /* TEST BEGIN */
  
  p4_pd_test_actionA_action_spec actionA_action_spec =
    {0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff};
  p4_pd_test_actionB_action_spec actionB_action_spec =
    {0xab};
  
  // right now PD assumes everything is passed in network byte order, so this
  // will actually be interpreted as byte string "bb00aa00"
  p4_pd_test_ExactOne_match_spec_t ExactOne_match_spec = {0x00aa00bb};
  p4_pd_test_ExactOne_table_add_with_actionA(sess_hdl, dev_tgt,
                                             &ExactOne_match_spec,
                                             &actionA_action_spec,
                                             &entry_hdl);

  p4_pd_test_ExactOne_table_modify_with_actionB(sess_hdl, dev_tgt.device_id,
                                                entry_hdl, &actionB_action_spec);

  p4_pd_counter_value_t counter_value;
  p4_pd_test_ExactOne_read_counter(sess_hdl, dev_tgt, entry_hdl, &counter_value);  

  p4_pd_test_ExactOne_reset_counters(sess_hdl, dev_tgt);

  p4_pd_test_ExactOne_table_delete(sess_hdl, dev_tgt.device_id, entry_hdl);
  
  p4_pd_test_LpmOne_match_spec_t LpmOne_match_spec = {0x12345678, 12};
  p4_pd_test_LpmOne_table_add_with_actionA(sess_hdl, dev_tgt,
                                           &LpmOne_match_spec,
                                           &actionA_action_spec,
                                           &entry_hdl);

  p4_pd_test_TernaryOne_match_spec_t TernaryOne_match_spec = {0x10101010,
                                                              0xff000a00};
  p4_pd_test_TernaryOne_table_add_with_actionA(sess_hdl, dev_tgt,
                                               &TernaryOne_match_spec,
                                               22 /* priority */,
                                               &actionA_action_spec,
                                               &entry_hdl);
  
  p4_pd_test_ExactOneNA_match_spec_t ExactOneNA_match_spec = {0xdebc0a};
  p4_pd_test_ExactOneNA_table_add_with_actionA(sess_hdl, dev_tgt,
                                               &ExactOneNA_match_spec,
                                               &actionA_action_spec,
                                               &entry_hdl);
  
  p4_pd_test_ExactTwo_match_spec_t ExactTwo_match_spec = {0xaabbccdd, 0xeeff};
  p4_pd_test_ExactTwo_table_add_with_actionA(sess_hdl, dev_tgt,
                                             &ExactTwo_match_spec,
                                             &actionA_action_spec,
                                             &entry_hdl);

  p4_pd_test_ExactAndValid_match_spec_t ExactAndValid_match_spec = {0xaabbccdd, 1u};
  p4_pd_test_ExactAndValid_table_add_with_actionA(sess_hdl, dev_tgt,
						  &ExactAndValid_match_spec,
						  &actionA_action_spec,
						  &entry_hdl);
  
  p4_pd_test_ExactOne_set_default_action_actionA(sess_hdl, dev_tgt,
                                                 &actionA_action_spec,
                                                 &entry_hdl);

  /* indirect table */

  p4_pd_mbr_hdl_t mbr_hdl;

  p4_pd_test_ActProf_add_member_with_actionA(sess_hdl, dev_tgt,
					     &actionA_action_spec,
					     &mbr_hdl);

  p4_pd_test_ActProf_modify_member_with_actionB(sess_hdl, dev_tgt.device_id,
						mbr_hdl,
						&actionB_action_spec);

  p4_pd_test_Indirect_match_spec_t Indirect_match_spec = {0xaabbccdd};
  p4_pd_test_Indirect_add_entry(sess_hdl, dev_tgt,
				&Indirect_match_spec, mbr_hdl,
				&entry_hdl);

  p4_pd_test_Indirect_table_delete(sess_hdl, dev_tgt.device_id, entry_hdl);

  p4_pd_test_Indirect_set_default_entry(sess_hdl, dev_tgt,
					mbr_hdl, &entry_hdl);

  p4_pd_test_ActProf_del_member(sess_hdl, dev_tgt.device_id, mbr_hdl);


  /* END TEST */

  p4_pd_test_remove_device(sess_hdl, dev_tgt.device_id);
  
  return 0;
}