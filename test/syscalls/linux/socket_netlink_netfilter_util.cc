// Copyright 2025 The gVisor Authors.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "test/syscalls/linux/socket_netlink_netfilter_util.h"

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include "gtest/gtest.h"
#include "test/syscalls/linux/socket_netlink_util.h"
#include "test/util/file_descriptor.h"
#include "test/util/posix_error.h"

namespace gvisor {
namespace testing {

// Helper function to initialize a nfgenmsg header.
void InitNetfilterGenmsg(struct nfgenmsg* genmsg, uint8_t family,
                         uint8_t version, uint16_t res_id) {
  genmsg->nfgen_family = family;
  genmsg->version = version;
  genmsg->res_id = res_id;
}

// Helper function to check the netfilter table attributes.
void CheckNetfilterTableAttributes(const NfTableCheckOptions& options) {
  // Check for the NFTA_TABLE_NAME attribute.
  const struct nfattr* table_name_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_TABLE_NAME);
  if (table_name_attr != nullptr && options.test_table_name != nullptr) {
    std::string name(reinterpret_cast<const char*>(NFA_DATA(table_name_attr)));
    EXPECT_EQ(name, options.test_table_name);
  } else {
    EXPECT_EQ(table_name_attr, nullptr);
    EXPECT_EQ(options.test_table_name, nullptr);
  }

  // Check for the NFTA_TABLE_USE attribute.
  const struct nfattr* table_use_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_TABLE_USE);
  if (table_use_attr != nullptr && options.expected_chain_count != nullptr) {
    uint32_t count = *(reinterpret_cast<uint32_t*>(NFA_DATA(table_use_attr)));
    EXPECT_EQ(count, *options.expected_chain_count);
  } else {
    EXPECT_EQ(table_use_attr, nullptr);
    EXPECT_EQ(options.expected_chain_count, nullptr);
  }

  if (!options.skip_handle_check) {
    // Check for the NFTA_TABLE_HANDLE attribute.
    const struct nfattr* handle_attr =
        FindNfAttr(options.hdr, nullptr, NFTA_TABLE_HANDLE);
    if (handle_attr != nullptr && options.expected_handle != nullptr) {
      uint64_t handle = *(reinterpret_cast<uint64_t*>(NFA_DATA(handle_attr)));
      EXPECT_EQ(handle, *options.expected_handle);
    } else {
      EXPECT_EQ(handle_attr, nullptr);
      EXPECT_EQ(options.expected_handle, nullptr);
    }
  }

  // Check for the NFTA_TABLE_FLAGS attribute.
  const struct nfattr* flags_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_TABLE_FLAGS);
  if (flags_attr != nullptr && options.expected_flags != nullptr) {
    uint32_t flags = *(reinterpret_cast<uint32_t*>(NFA_DATA(flags_attr)));
    EXPECT_EQ(flags, *options.expected_flags);
  } else {
    EXPECT_EQ(flags_attr, nullptr);
    EXPECT_EQ(options.expected_flags, nullptr);
  }

  // Check for the NFTA_TABLE_OWNER attribute.
  const struct nfattr* owner_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_TABLE_OWNER);
  if (owner_attr != nullptr) {
    uint32_t owner = *(reinterpret_cast<uint32_t*>(NFA_DATA(owner_attr)));
    EXPECT_EQ(owner, *options.expected_owner);
  } else {
    EXPECT_EQ(owner_attr, nullptr);
    EXPECT_EQ(options.expected_owner, nullptr);
  }

  // Check for the NFTA_TABLE_USERDATA attribute.
  const struct nfattr* user_data_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_TABLE_USERDATA);

  if (user_data_attr != nullptr && options.expected_udata_size != nullptr) {
    uint8_t user_data[VALID_USERDATA_SIZE] = {};
    EXPECT_EQ(user_data_attr->nfa_len - NLA_HDRLEN,
              *options.expected_udata_size);
    std::memcpy(user_data, NFA_DATA(user_data_attr),
                *options.expected_udata_size);
    EXPECT_EQ(
        memcmp(user_data, options.expected_udata, *options.expected_udata_size),
        0);
  } else {
    EXPECT_EQ(user_data_attr, nullptr);
    EXPECT_EQ(options.expected_udata_size, nullptr);
  }
}

// Helper function to check the netfilter chain attributes.
void CheckNetfilterChainAttributes(const NfChainCheckOptions& options) {
  // Check for the NFTA_CHAIN_TABLE attribute.
  const struct nfattr* table_name_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_TABLE);
  if (table_name_attr != nullptr && options.expected_table_name != nullptr) {
    std::string table_name(
        reinterpret_cast<const char*>(NFA_DATA(table_name_attr)));
    EXPECT_EQ(table_name, options.expected_table_name);
  } else {
    EXPECT_EQ(table_name_attr, nullptr);
    EXPECT_EQ(options.expected_table_name, nullptr);
  }

  // Check for the NFTA_CHAIN_NAME attribute.
  const struct nfattr* chain_name_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_NAME);
  if (chain_name_attr != nullptr && options.expected_chain_name != nullptr) {
    std::string chain_name(
        reinterpret_cast<const char*>(NFA_DATA(chain_name_attr)));
    EXPECT_EQ(chain_name, options.expected_chain_name);
  } else {
    EXPECT_EQ(chain_name_attr, nullptr);
    EXPECT_EQ(options.expected_chain_name, nullptr);
  }

  if (!options.skip_handle_check) {
    // Check for the NFTA_CHAIN_HANDLE attribute.
    const struct nfattr* handle_attr =
        FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_HANDLE);
    if (handle_attr != nullptr && options.expected_handle != nullptr) {
      uint64_t handle = *(reinterpret_cast<uint64_t*>(NFA_DATA(handle_attr)));
      EXPECT_EQ(handle, *options.expected_handle);
    } else {
      EXPECT_EQ(handle_attr, nullptr);
      EXPECT_EQ(options.expected_handle, nullptr);
    }
  }

  // Check for the NFTA_CHAIN_POLICY attribute.
  const struct nfattr* policy_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_POLICY);
  if (policy_attr != nullptr && options.expected_policy != nullptr) {
    uint32_t policy = *(reinterpret_cast<uint32_t*>(NFA_DATA(policy_attr)));
    EXPECT_EQ(policy, *options.expected_policy);
  } else {
    EXPECT_EQ(policy_attr, nullptr);
    EXPECT_EQ(options.expected_policy, nullptr);
  }

  // Check for the NFTA_CHAIN_TYPE attribute.
  const struct nfattr* chain_type_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_TYPE);
  if (chain_type_attr != nullptr && options.expected_chain_type != nullptr) {
    std::string chain_type(
        reinterpret_cast<const char*>(NFA_DATA(chain_type_attr)));
    EXPECT_EQ(chain_type, options.expected_chain_type);
  } else {
    EXPECT_EQ(chain_type_attr, nullptr);
    EXPECT_EQ(options.expected_chain_type, nullptr);
  }

  // Check for the NFTA_CHAIN_FLAGS attribute.
  const struct nfattr* flags_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_FLAGS);
  if (flags_attr != nullptr && options.expected_flags != nullptr) {
    uint32_t flags = *(reinterpret_cast<uint32_t*>(NFA_DATA(flags_attr)));
    EXPECT_EQ(flags, *options.expected_flags);
  } else {
    EXPECT_EQ(flags_attr, nullptr);
    EXPECT_EQ(options.expected_flags, nullptr);
  }

  // Check for the NFTA_CHAIN_USE attribute.
  const struct nfattr* use_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_USE);
  if (use_attr != nullptr && options.expected_use != nullptr) {
    uint32_t use = *(reinterpret_cast<uint32_t*>(NFA_DATA(use_attr)));
    EXPECT_EQ(use, *options.expected_use);
  } else {
    EXPECT_EQ(use_attr, nullptr);
    EXPECT_EQ(options.expected_use, nullptr);
  }

  // Check for the NFTA_CHAIN_USERDATA attribute.
  const struct nfattr* user_data_attr =
      FindNfAttr(options.hdr, nullptr, NFTA_CHAIN_USERDATA);

  if (user_data_attr != nullptr && options.expected_udata_size != nullptr) {
    uint8_t user_data[VALID_USERDATA_SIZE] = {};
    EXPECT_EQ(user_data_attr->nfa_len - NLA_HDRLEN,
              *options.expected_udata_size);
    std::memcpy(user_data, NFA_DATA(user_data_attr),
                *options.expected_udata_size);
    EXPECT_EQ(
        memcmp(user_data, options.expected_udata, *options.expected_udata_size),
        0);
  } else {
    EXPECT_EQ(user_data_attr, nullptr);
    EXPECT_EQ(options.expected_udata_size, nullptr);
  }
}

// Helper function to add a default table.
void AddDefaultTable(const AddDefaultTableOptions options) {
  const char* test_table_name = options.test_table_name;
  if (test_table_name == nullptr) {
    test_table_name = DEFAULT_TABLE_NAME;
  }

  std::vector<char> add_table_request_buffer =
      NlReq("newtable req ack inet")
          .Seq(options.seq)
          .StrAttr(NFTA_TABLE_NAME, test_table_name)
          .Build();
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(options.fd, options.seq,
                                           add_table_request_buffer.data(),
                                           add_table_request_buffer.size()));
}

// Helper function to add a default base chain.
void AddDefaultBaseChain(const AddDefaultBaseChainOptions options) {
  const char* test_table_name = options.test_table_name;
  if (test_table_name == nullptr) {
    test_table_name = DEFAULT_TABLE_NAME;
  }

  const char* test_chain_name = options.test_chain_name;
  if (test_chain_name == nullptr) {
    test_chain_name = DEFAULT_CHAIN_NAME;
  }

  const char test_chain_type_name[] = "filter";
  const uint32_t test_policy = NF_ACCEPT;
  const uint32_t test_hook_num = NF_INET_PRE_ROUTING;
  const uint32_t test_hook_priority = 0;
  const uint32_t test_chain_flags = NFT_CHAIN_BASE;

  std::vector<char> nested_hook_data =
      NlNestedAttr()
          .U32Attr(NFTA_HOOK_HOOKNUM, &test_hook_num)
          .U32Attr(NFTA_HOOK_PRIORITY, &test_hook_priority)
          .StrAttr(NFTA_CHAIN_TYPE, test_chain_type_name)
          .Build();
  std::vector<char> add_chain_request_buffer =
      NlReq("newchain req ack inet")
          .Seq(options.seq)
          .StrAttr(NFTA_CHAIN_TABLE, test_table_name)
          .StrAttr(NFTA_CHAIN_NAME, test_chain_name)
          .U32Attr(NFTA_CHAIN_POLICY, &test_policy)
          .RawAttr(NFTA_CHAIN_HOOK, nested_hook_data.data(),
                   nested_hook_data.size())
          .U32Attr(NFTA_CHAIN_FLAGS, &test_chain_flags)
          .Build();
  ASSERT_NO_ERRNO(NetlinkRequestAckOrError(options.fd, options.seq,
                                           add_chain_request_buffer.data(),
                                           add_chain_request_buffer.size()));
}

NlReq& NlReq::MsgType(uint8_t msg_type) {
  EXPECT_FALSE(msg_type_set_) << "Message type already set: " << msg_type_;

  msg_type_ = msg_type;
  msg_type_set_ = true;
  return *this;
}

NlReq& NlReq::Flags(uint16_t flags) {
  flags_ = flags;
  return *this;
}

NlReq& NlReq::Seq(uint32_t seq) {
  seq_ = seq;
  return *this;
}

NlReq& NlReq::Family(uint8_t family) {
  EXPECT_FALSE(family_set_) << "Family already set: " << family_;

  family_ = family;
  family_set_ = true;
  return *this;
}

// Constructor that parses a string into a NlReq object with the header
// filled in.
NlReq::NlReq(const std::string& str) {
  std::stringstream ss(str);
  std::string token;
  // Skips leading and trailing whitespace.
  while (ss >> token) {
    if (MsgTypeToken(token)) {
      continue;
    } else if (FlagsToken(token)) {
      continue;
    } else if (FamilyToken(token)) {
      continue;
    } else {
      ADD_FAILURE() << "Unknown token: " << token;
    }
  }
}

bool NlReq::MsgTypeToken(const std::string& token) {
  std::map<std::string, uint8_t> token_to_msg_type = {
      {"newtable", NFT_MSG_NEWTABLE}, {"gettable", NFT_MSG_GETTABLE},
      {"deltable", NFT_MSG_DELTABLE}, {"destroytable", NFT_MSG_DESTROYTABLE},
      {"newchain", NFT_MSG_NEWCHAIN}, {"getchain", NFT_MSG_GETCHAIN},
      {"delchain", NFT_MSG_DELCHAIN}, {"destroychain", NFT_MSG_DESTROYCHAIN},
      {"newrule", NFT_MSG_NEWRULE}};
  auto it = token_to_msg_type.find(token);
  if (it != token_to_msg_type.end()) {
    EXPECT_FALSE(msg_type_set_) << "Message type already set: " << msg_type_;
    msg_type_ = it->second;
    msg_type_set_ = true;
    return true;
  }
  return false;
}

bool NlReq::FlagsToken(const std::string& token) {
  std::map<std::string, uint16_t> token_to_flags = {
      {"req", NLM_F_REQUEST},   {"ack", NLM_F_ACK},
      {"dump", NLM_F_DUMP},     {"replace", NLM_F_REPLACE},
      {"excl", NLM_F_EXCL},     {"nonrec", NLM_F_NONREC},
      {"create", NLM_F_CREATE}, {"append", NLM_F_APPEND}};
  auto it = token_to_flags.find(token);
  if (it != token_to_flags.end()) {
    flags_ |= it->second;
    return true;
  }
  return false;
}

bool NlReq::FamilyToken(const std::string& token) {
  std::map<std::string, uint8_t> token_to_family = {
      {"unspec", NFPROTO_UNSPEC}, {"inet", NFPROTO_INET},
      {"ipv4", NFPROTO_IPV4},     {"ipv6", NFPROTO_IPV6},
      {"arp", NFPROTO_ARP},       {"bridge", NFPROTO_BRIDGE},
      {"netdev", NFPROTO_NETDEV},
  };
  auto it = token_to_family.find(token);
  if (it != token_to_family.end()) {
    EXPECT_FALSE(family_set_) << "Family already set: " << family_;
    family_ = it->second;
    family_set_ = true;
    return true;
  }
  return false;
}

// Method to add an attribute to the message. payload_size must be the size of
// the payload in bytes.
NlReq& NlReq::RawAttr(uint16_t attr_type, const void* payload,
                      size_t payload_size) {
  // Store a pointer to the payload and the size to construct it later.
  attributes_[attr_type] = {reinterpret_cast<const char*>(payload),
                            payload_size};
  return *this;
}

// Method to add a string attribute to the message.
// The payload is expected to be a null-terminated string.
NlReq& NlReq::StrAttr(uint16_t attr_type, const char* payload) {
  return RawAttr(attr_type, payload, strlen(payload) + 1);
}

// Method to add a uint8_t attribute to the message.
NlReq& NlReq::U8Attr(uint16_t attr_type, const uint8_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint8_t));
}

// Method to add a uint16_t attribute to the message.
NlReq& NlReq::U16Attr(uint16_t attr_type, const uint16_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint16_t));
}

// Method to add a uint32_t attribute to the message.
NlReq& NlReq::U32Attr(uint16_t attr_type, const uint32_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint32_t));
}

// Method to add a uint64_t attribute to the message.
NlReq& NlReq::U64Attr(uint16_t attr_type, const uint64_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint64_t));
}

std::vector<char> NlReq::Build() {
  size_t aligned_hdr_size = NLMSG_ALIGN(sizeof(nlmsghdr));
  size_t aligned_genmsg_size = NLMSG_ALIGN(sizeof(nfgenmsg));
  size_t total_attr_size = 0;

  for (const auto& [attr_type, attr_data] : attributes_) {
    const auto& [_, payload_size] = attr_data;
    total_attr_size += NLA_ALIGN(NLA_HDRLEN + payload_size);
  }

  size_t total_message_len =
      NLMSG_ALIGN(aligned_hdr_size + aligned_genmsg_size + total_attr_size);

  msg_buffer_.resize(total_message_len);
  std::memset(msg_buffer_.data(), 0, total_message_len);

  struct nlmsghdr* nlh = reinterpret_cast<struct nlmsghdr*>(msg_buffer_.data());
  InitNetlinkHdr(nlh, (uint32_t)total_message_len,
                 MakeNetlinkMsgType(NFNL_SUBSYS_NFTABLES, msg_type_), seq_,
                 flags_);

  struct nfgenmsg* nfg = reinterpret_cast<struct nfgenmsg*>(NLMSG_DATA(nlh));
  InitNetfilterGenmsg(nfg, family_, NFNETLINK_V0, 0);

  char* payload =
      (char*)msg_buffer_.data() + aligned_hdr_size + aligned_genmsg_size;

  for (const auto& [attr_type, attr_data] : attributes_) {
    const auto& [payload_data, payload_size] = attr_data;
    struct nlattr* attr = reinterpret_cast<struct nlattr*>(payload);
    InitNetlinkAttr(attr, payload_size, attr_type);
    std::memcpy((char*)attr + NLA_HDRLEN, payload_data, payload_size);
    // Move over to the next attribute.
    payload += NLA_ALIGN(NLA_HDRLEN + payload_size);
  }
  return msg_buffer_;
}

// Method to add an attribute to the message. payload_size must be the size of
// the payload in bytes.
NlNestedAttr& NlNestedAttr::RawAttr(uint16_t attr_type, const void* payload,
                                    size_t payload_size) {
  // Store a pointer to the payload and the size to construct it later.
  attributes_[attr_type] = {reinterpret_cast<const char*>(payload),
                            payload_size};
  return *this;
}

// Method to add a string attribute to the message.
// The payload is expected to be a null-terminated string.
NlNestedAttr& NlNestedAttr::StrAttr(uint16_t attr_type, const char* payload) {
  return RawAttr(attr_type, payload, strlen(payload) + 1);
}

// Method to add a uint8_t attribute to the message.
NlNestedAttr& NlNestedAttr::U8Attr(uint16_t attr_type, const uint8_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint8_t));
}

// Method to add a uint16_t attribute to the message.
NlNestedAttr& NlNestedAttr::U16Attr(uint16_t attr_type,
                                    const uint16_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint16_t));
}

// Method to add a uint32_t attribute to the message.
NlNestedAttr& NlNestedAttr::U32Attr(uint16_t attr_type,
                                    const uint32_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint32_t));
}

// Method to add a uint64_t attribute to the message.
NlNestedAttr& NlNestedAttr::U64Attr(uint16_t attr_type,
                                    const uint64_t* payload) {
  return RawAttr(attr_type, payload, sizeof(uint64_t));
}

std::vector<char> NlNestedAttr::Build() {
  size_t total_attr_size = 0;

  for (const auto& [attr_type, attr_data] : attributes_) {
    const auto& [_, payload_size] = attr_data;
    total_attr_size += NLA_ALIGN(NLA_HDRLEN + payload_size);
  }

  msg_buffer_.resize(total_attr_size);
  std::memset(msg_buffer_.data(), 0, total_attr_size);

  char* payload = (char*)msg_buffer_.data();

  for (const auto& [attr_type, attr_data] : attributes_) {
    const auto& [payload_data, payload_size] = attr_data;
    struct nlattr* attr = reinterpret_cast<struct nlattr*>(payload);
    InitNetlinkAttr(attr, payload_size, attr_type);
    std::memcpy((char*)attr + NLA_HDRLEN, payload_data, payload_size);
    // Move over to the next attribute.
    payload += NLA_ALIGN(NLA_HDRLEN + payload_size);
  }
  return msg_buffer_;
}

NlListAttr& NlListAttr::Add(const std::vector<char>& attr) {
  nested_attrs_.push_back(attr);
  return *this;
}

// Builds the list attribute with the maximum number of attributes + 1.
// This will return an invalid message if used.
std::vector<char> NlListAttr::BuildWithMaxAttrs() {
  NlListAttr list_attr;
  std::vector<char> expr = NlImmExpr::DefaultAcceptAll();
  for (int i = 0; i < kMaxExprs + 1; ++i) {
    list_attr.Add(expr);
  }

  return list_attr.Build();
}

std::vector<char> NlListAttr::Build() {
  size_t total_message_size = 0;
  for (const auto& attr : nested_attrs_) {
    total_message_size += NLA_ALIGN(NLA_HDRLEN + attr.size());
  }

  msg_buffer_.resize(total_message_size);
  std::memset(msg_buffer_.data(), 0, total_message_size);

  char* payload = reinterpret_cast<char*>(msg_buffer_.data());
  for (const auto& attr : nested_attrs_) {
    struct nlattr* list_elem_attr = reinterpret_cast<struct nlattr*>(payload);
    InitNetlinkAttr(list_elem_attr, attr.size(), NFTA_LIST_ELEM);
    std::memcpy(reinterpret_cast<char*>(list_elem_attr) + NLA_HDRLEN,
                attr.data(), attr.size());
    payload += NLA_ALIGN(NLA_HDRLEN + attr.size());
  }
  return msg_buffer_;
}

NlImmExpr& NlImmExpr::Dreg(uint32_t dreg) {
  dreg_ = dreg;
  return *this;
}

NlImmExpr& NlImmExpr::VerdictCode(uint32_t verdict_code) {
  verdict_code_ = verdict_code;
  return *this;
}

NlImmExpr& NlImmExpr::Value(const std::vector<char>& value) {
  value_ = value;
  return *this;
}

std::vector<char> NlImmExpr::VerdictBuild() {
  std::vector<char> verdict_code_data =
      NlNestedAttr().U32Attr(NFTA_VERDICT_CODE, &verdict_code_).Build();
  std::vector<char> immediate_data =
      NlNestedAttr()
          .RawAttr(NFTA_DATA_VERDICT, verdict_code_data.data(),
                   verdict_code_data.size())
          .Build();
  std::vector<char> immediate_attrs =
      NlNestedAttr()
          .U32Attr(NFTA_IMMEDIATE_DREG, &dreg_)
          .RawAttr(NFTA_IMMEDIATE_DATA, immediate_data.data(),
                   immediate_data.size())
          .Build();
  return NlNestedAttr()
      .StrAttr(NFTA_EXPR_NAME, "immediate")
      .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(), immediate_attrs.size())
      .Build();
}

std::vector<char> NlImmExpr::ValueBuild() {
  std::vector<char> immediate_data =
      NlNestedAttr()
          .RawAttr(NFTA_DATA_VALUE, value_.data(), value_.size())
          .Build();
  std::vector<char> immediate_attrs =
      NlNestedAttr()
          .U32Attr(NFTA_IMMEDIATE_DREG, &dreg_)
          .RawAttr(NFTA_IMMEDIATE_DATA, immediate_data.data(),
                   immediate_data.size())
          .Build();
  return NlNestedAttr()
      .StrAttr(NFTA_EXPR_NAME, "immediate")
      .RawAttr(NFTA_EXPR_DATA, immediate_attrs.data(), immediate_attrs.size())
      .Build();
}

std::vector<char> NlImmExpr::DefaultAcceptAll() {
  return NlImmExpr()
      .Dreg(NFT_REG_VERDICT)
      .VerdictCode(NF_ACCEPT)
      .VerdictBuild();
}

std::vector<char> NlImmExpr::DefaultDropAll() {
  return NlImmExpr().Dreg(NFT_REG_VERDICT).VerdictCode(NF_DROP).VerdictBuild();
}

}  // namespace testing
}  // namespace gvisor
