// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "fastboot.h"

#include <lib/abr/abr.h>
#include <lib/abr/data.h>
#include <lib/abr/util.h>
#include <lib/fastboot/test/test-transport.h>
#include <lib/zircon_boot/test/mock_zircon_boot_ops.h>

#include <vector>

#include <gtest/gtest.h>

#include "backends.h"
#include "mock_boot_service.h"
#include "utils.h"

// For "ABC"sv literal string view operator
using namespace std::literals;

namespace gigaboot {

RebootMode test_reboot_mode;
bool set_reboot_mode_res = true;
bool SetRebootMode(RebootMode mode) {
  test_reboot_mode = mode;
  return set_reboot_mode_res;
}

class FakeRebootMode {
 public:
  FakeRebootMode(RebootMode test_mode, bool reboot_mode_res = true) {
    test_reboot_mode = test_mode;
    set_reboot_mode_res = reboot_mode_res;
  }

  ~FakeRebootMode() {
    test_reboot_mode = RebootMode::kNormal;
    set_reboot_mode_res = true;
  }
};

namespace {

class TestTcpTransport : public TcpTransportInterface {
 public:
  // Add data to the input stream.
  void AddInputData(const void* data, size_t size) {
    const uint8_t* start = static_cast<const uint8_t*>(data);
    in_data_.insert(in_data_.end(), start, start + size);
  }

  // Add a fastboot packet (length-prefixed byte sequence) to input stream.
  void AddFastbootPacket(const void* data, uint64_t size) {
    uint64_t be_size = ToBigEndian(size);
    AddInputData(&be_size, sizeof(be_size));
    AddInputData(data, size);
  }

  bool Read(void* out, size_t size) override {
    if (offset_ + size > in_data_.size()) {
      return false;
    }

    memcpy(out, in_data_.data() + offset_, size);
    offset_ += size;
    return true;
  }

  bool Write(const void* data, size_t size) override {
    const uint8_t* start = static_cast<const uint8_t*>(data);
    out_data_.insert(out_data_.end(), start, start + size);
    return true;
  }

  const std::vector<uint8_t>& GetOutData() { return out_data_; }

  void PopOutput(size_t size) {
    ASSERT_GE(out_data_.size(), size);
    out_data_ = std::vector<uint8_t>(out_data_.begin() + size, out_data_.end());
  }

  void PopAndCheckOutput(std::string_view expected) {
    ASSERT_GE(out_data_.size(), expected.size());
    std::string_view actual(reinterpret_cast<char*>(out_data_.data()), expected.size());
    ASSERT_EQ(actual, expected);
    PopOutput(expected.size());
  }

 private:
  size_t offset_ = 0;
  std::vector<uint8_t> in_data_;
  std::vector<uint8_t> out_data_;
};

constexpr size_t kDownloadBufferSize = 1024;
uint8_t download_buffer[kDownloadBufferSize];

void CheckPacketsEqual(const fastboot::Packets& lhs, const fastboot::Packets& rhs) {
  ASSERT_EQ(lhs.size(), rhs.size());
  for (size_t i = 0; i < lhs.size(); i++) {
    ASSERT_EQ(lhs[i], rhs[i]);
  }
}

TEST(FastbootTest, FastbootContinueTest) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  fastboot::TestTransport transport;
  transport.AddInPacket(std::string("continue"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  ASSERT_EQ(transport.GetOutPackets().size(), 1ULL);
  ASSERT_EQ(transport.GetOutPackets().back(), "OKAY");
  ASSERT_TRUE(fastboot.IsContinue());
}

struct BasicVarTestCase {
  char const* name;
  char const* var;
  char const* expected_val;
};

using BasicVarTest = ::testing::TestWithParam<BasicVarTestCase>;

TEST_P(BasicVarTest, TestBasicVar) {
  BasicVarTestCase const& test_case = GetParam();

  MockZirconBootOps zb_ops;
  Fastboot fastboot(download_buffer, zb_ops.GetZirconBootOps());
  fastboot::TestTransport transport;

  std::string command = std::string{"getvar:"} + test_case.var;
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(
      transport.GetOutPackets(), fastboot::Packets{std::string{"OKAY"} + test_case.expected_val}));
}

INSTANTIATE_TEST_SUITE_P(FastbootGetVarsTests, BasicVarTest,
                         testing::ValuesIn<BasicVarTest::ParamType>({
                             {"slot_count", "slot-count", "2"},
                             {"slot_suffixes", "slot-suffixes", "a,b"},
                             {"max_download_size", "max-download-size", "0x00000400"},
                         }),
                         [](testing::TestParamInfo<BasicVarTest::ParamType> const& info) {
                           return info.param.name;
                         });

TEST(FastbootTcpSessionTest, ExitOnFastbootContinue) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  TestTcpTransport transport;

  // Handshake message
  const char handshake_message[] = "FB01";
  transport.AddInputData(handshake_message, strlen(handshake_message));

  // fastboot continue
  const char fastboot_cmd_continue[] = "continue";
  transport.AddFastbootPacket(fastboot_cmd_continue, strlen(fastboot_cmd_continue));

  // Add another packet, which should not be processed.
  const char fastboot_cmd_not_processed[] = "not-processed";
  transport.AddFastbootPacket(fastboot_cmd_not_processed, strlen(fastboot_cmd_not_processed));

  FastbootTcpSession(transport, fastboot);

  // API should return the same "FB01"
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FB01"));

  // Continue should return "OKAY"
  ASSERT_NO_FATAL_FAILURE(
      transport.PopAndCheckOutput("\x00\x00\x00\x00\x00\x00\x00\x04"
                                  "OKAY"sv));

  // The 'not-processed' command packet should not be processed. We shouldn't get
  // any new data in the output. (in this case it will be a failure message if processed).
  ASSERT_TRUE(transport.GetOutData().empty());
}

TEST(FastbootTcpSessionTest, HandshakeFailsNotFB) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  TestTcpTransport transport;

  // Handshake message
  const char handshake_message[] = "AC01";
  transport.AddInputData(handshake_message, strlen(handshake_message));

  FastbootTcpSession(transport, fastboot);

  // API should write the same "FB01" no matter what is received
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FB01"));

  // Nothing should have been written.
  ASSERT_TRUE(transport.GetOutData().empty());
}

TEST(FastbootTcpSessionTest, HandshakeFailsNotNumericVersion) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  TestTcpTransport transport;

  // Handshake message
  const char handshake_message[] = "FBxx";
  transport.AddInputData(handshake_message, strlen(handshake_message));

  FastbootTcpSession(transport, fastboot);

  // API should write the same "FB01" no matter what is received
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FB01"));

  // Nothing should have been written.
  ASSERT_TRUE(transport.GetOutData().empty());
}

TEST(FastbootTcpSessionTest, ExitWhenNoMoreData) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  TestTcpTransport transport;

  // Handshake message
  const char handshake_message[] = "FB01";
  transport.AddInputData(handshake_message, strlen(handshake_message));

  FastbootTcpSession(transport, fastboot);

  // API should returns the same "FB01"
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FB01"));

  // No more data should be written.
  ASSERT_TRUE(transport.GetOutData().empty());
}

TEST(FastbootTcpSessionTest, ExitOnCommandFailure) {
  Fastboot fastboot(download_buffer, ZirconBootOps());
  TestTcpTransport transport;

  // Handshake message
  const char handshake_message[] = "FB01";
  transport.AddInputData(handshake_message, strlen(handshake_message));

  // fastboot continue
  const char fastboot_cmd_continue[] = "unknown-cmd";
  transport.AddFastbootPacket(fastboot_cmd_continue, strlen(fastboot_cmd_continue));

  FastbootTcpSession(transport, fastboot);

  // Check and skip handshake message
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FB01"));
  // Skips 8- bytes length prefix
  ASSERT_NO_FATAL_FAILURE(transport.PopOutput(8));
  ASSERT_NO_FATAL_FAILURE(transport.PopAndCheckOutput("FAIL"));
}

class FastbootFlashTest : public ::testing::Test {
 public:
  FastbootFlashTest()
      : image_device_({"path-A", "path-B", "path-C", "image"}),
        block_device_({"path-A", "path-B", "path-C"}, 1024) {
    stub_service_.AddDevice(&image_device_);

    // Add a block device for fastboot flash test.
    stub_service_.AddDevice(&block_device_);
    block_device_.InitializeGpt();
  }

  void AddPartition(const gpt_entry_t& new_entry) { block_device_.AddGptPartition(new_entry); }

  uint8_t* BlockDeviceStart() { return block_device_.fake_disk_io_protocol().contents(0).data(); }

  // A helper to download data to fastboot
  void DownloadData(Fastboot& fastboot, const std::vector<uint8_t>& download_content) {
    char download_command[fastboot::kMaxCommandPacketSize];
    snprintf(download_command, fastboot::kMaxCommandPacketSize, "download:%08zx",
             download_content.size());
    fastboot::TestTransport transport;
    transport.AddInPacket(std::string(download_command));
    zx::status ret = fastboot.ProcessPacket(&transport);
    ASSERT_TRUE(ret.is_ok());

    // Download
    transport.AddInPacket(download_content);
    ret = fastboot.ProcessPacket(&transport);
    ASSERT_TRUE(ret.is_ok());

    char expected_data_message[fastboot::kMaxCommandPacketSize];
    snprintf(expected_data_message, fastboot::kMaxCommandPacketSize, "DATA%08zx",
             download_content.size());
    CheckPacketsEqual(transport.GetOutPackets(), {
                                                     expected_data_message,
                                                     "OKAY",
                                                 });
  }

  MockStubService& stub_service() { return stub_service_; }
  Device& image_device() { return image_device_; }
  BlockDevice& block_device() { return block_device_; }
  MockZirconBootOps& mock_zb_ops() { return mock_zb_ops_; }

 private:
  MockStubService stub_service_;
  Device image_device_;
  BlockDevice block_device_;
  MockZirconBootOps mock_zb_ops_;
};

class FastbootSlotTest : public ::testing::Test {
 public:
  void InitializeAbr(AbrSlotIndex slot) {
    mock_zb_ops_.AddPartition(GPT_DURABLE_BOOT_NAME, sizeof(AbrData));
    ZirconBootOps zb_ops = mock_zb_ops_.GetZirconBootOps();
    AbrOps abr_ops = GetAbrOpsFromZirconBootOps(&zb_ops);
    AbrResult res;

    if (slot == kAbrSlotIndexR) {
      res = AbrMarkSlotUnbootable(&abr_ops, kAbrSlotIndexA);
      ASSERT_EQ(res, kAbrResultOk);
      res = AbrMarkSlotUnbootable(&abr_ops, kAbrSlotIndexB);
      ASSERT_EQ(res, kAbrResultOk);
    } else {
      res = AbrMarkSlotActive(&abr_ops, slot);
      ASSERT_EQ(res, kAbrResultOk);
    }
  }

  void MarkUnbootable(AbrSlotIndex slot) {
    mock_zb_ops_.AddPartition(GPT_DURABLE_BOOT_NAME, sizeof(AbrData));
    ZirconBootOps zb_ops = mock_zb_ops_.GetZirconBootOps();
    AbrOps abr_ops = GetAbrOpsFromZirconBootOps(&zb_ops);
    AbrResult res = AbrMarkSlotUnbootable(&abr_ops, slot);
    ASSERT_EQ(res, kAbrResultOk);
  }

  void MarkSuccesful(AbrSlotIndex slot) {
    mock_zb_ops_.AddPartition(GPT_DURABLE_BOOT_NAME, sizeof(AbrData));
    ZirconBootOps zb_ops = mock_zb_ops_.GetZirconBootOps();
    AbrOps abr_ops = GetAbrOpsFromZirconBootOps(&zb_ops);
    AbrResult res = AbrMarkSlotSuccessful(&abr_ops, slot);
    ASSERT_EQ(res, kAbrResultOk);
  }

  MockZirconBootOps& mock_zb_ops() { return mock_zb_ops_; }

 private:
  MockZirconBootOps mock_zb_ops_;
};

struct FastbootSlotTestCase {
  AbrSlotIndex slot_index;
  char const* slot_str;
};

class FastbootSlotAllSlotsTest : public FastbootSlotTest,
                                 public testing::WithParamInterface<FastbootSlotTestCase> {};

TEST_P(FastbootSlotAllSlotsTest, TestFastbootGetSlot) {
  FastbootSlotTestCase const& test_case = GetParam();

  ZirconBootOps zb_ops = mock_zb_ops().GetZirconBootOps();
  Fastboot fastboot(download_buffer, zb_ops);
  fastboot::TestTransport transport;

  InitializeAbr(test_case.slot_index);
  transport.AddInPacket(std::string("getvar:current-slot"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  fastboot::Packets expected_packets = {std::string{"OKAY"} + test_case.slot_str};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));
}

INSTANTIATE_TEST_SUITE_P(
    FastbootSlotAllSlotsTests, FastbootSlotAllSlotsTest,
    testing::ValuesIn<FastbootSlotAllSlotsTest::ParamType>({
        {kAbrSlotIndexA, "a"},
        {kAbrSlotIndexB, "b"},
        {kAbrSlotIndexR, "r"},
    }),
    [](testing::TestParamInfo<FastbootSlotAllSlotsTest::ParamType> const& info) {
      return info.param.slot_str;
    });

using FastbootSlotABTest = FastbootSlotAllSlotsTest;

TEST_P(FastbootSlotABTest, TestFastbootSetActive) {
  FastbootSlotTestCase const& test_case = GetParam();

  ZirconBootOps zb_ops = mock_zb_ops().GetZirconBootOps();
  Fastboot fastboot(download_buffer, zb_ops);
  fastboot::TestTransport transport;

  InitializeAbr(kAbrSlotIndexR);
  transport.AddInPacket(std::string{"set_active:"} + test_case.slot_str);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  fastboot::Packets expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));
  transport.ClearOutPackets();

  transport.AddInPacket(std::string("getvar:current-slot"));
  ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  expected_packets[0] += test_case.slot_str;
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));
}

TEST_P(FastbootSlotABTest, GetVarSlotLastSetActive) {
  FastbootSlotTestCase const& test_case = GetParam();

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  InitializeAbr(test_case.slot_index);
  transport.AddInPacket(std::string("getvar:slot-last-set-active"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  fastboot::Packets expected_packets{std::string{"OKAY"} + test_case.slot_str};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));
}

TEST_P(FastbootSlotABTest, GetVarSlotUnbootable) {
  FastbootSlotTestCase const& test_case = GetParam();

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  MarkSuccesful(test_case.slot_index);

  std::string command = std::string{"getvar:slot-unbootable:"} + test_case.slot_str;
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYno"}));

  MarkUnbootable(test_case.slot_index);
  transport.ClearOutPackets();
  transport.AddInPacket(command);
  ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYyes"}));
}

TEST_P(FastbootSlotABTest, GetVarSlotRetryCount) {
  FastbootSlotTestCase const& test_case = GetParam();

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  InitializeAbr(test_case.slot_index);
  std::string command = std::string("getvar:slot-retry-count:") + test_case.slot_str;
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAY7"}));

  MarkUnbootable(test_case.slot_index);
  transport.ClearOutPackets();

  transport.AddInPacket(command);
  ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAY0"}));
}

TEST_P(FastbootSlotABTest, GetVarSlotSuccessful) {
  FastbootSlotTestCase const& test_case = GetParam();

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  MarkSuccesful(test_case.slot_index);
  std::string command = std::string{"getvar:slot-successful:"} + test_case.slot_str;
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYyes"}));

  MarkUnbootable(test_case.slot_index);
  transport.ClearOutPackets();
  transport.AddInPacket(command);
  ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYno"}));
}

INSTANTIATE_TEST_SUITE_P(FastbootSlotABTests, FastbootSlotABTest,
                         testing::ValuesIn<FastbootSlotABTest::ParamType>({
                             {kAbrSlotIndexA, "a"},
                             {kAbrSlotIndexB, "b"},
                         }),
                         [](testing::TestParamInfo<FastbootSlotABTest::ParamType> const& info) {
                           return info.param.slot_str;
                         });

TEST_F(FastbootSlotTest, GetVarSlotSuccessfulR) {
  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;
  InitializeAbr(kAbrSlotIndexR);

  std::string command = std::string{"getvar:slot-successful:r"};
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYyes"}));
}

TEST_F(FastbootSlotTest, GetVarSlotBootableR) {
  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;
  InitializeAbr(kAbrSlotIndexR);

  std::string command = std::string{"getvar:slot-unbootable:r"};
  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), {"OKAYno"}));
}

struct FastbootSetActiveErrorTestCase {
  char const* name;
  char const* user_str;
};

class FastbootSetActiveUserErrorTest
    : public FastbootSlotTest,
      public testing::WithParamInterface<FastbootSetActiveErrorTestCase> {};

TEST_P(FastbootSetActiveUserErrorTest, TestFastbootSetActiveUserError) {
  FastbootSetActiveErrorTestCase const& test_case = GetParam();

  InitializeAbr(kAbrSlotIndexR);
  ZirconBootOps zb_ops = mock_zb_ops().GetZirconBootOps();
  Fastboot fastboot(download_buffer, zb_ops);
  fastboot::TestTransport transport;

  std::string cmd = std::string{"set_active"} + test_case.user_str;
  transport.AddInPacket(cmd);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  auto sent_packets = transport.GetOutPackets();
  ASSERT_EQ(sent_packets.size(), 1ULL);
  ASSERT_EQ(sent_packets[0].compare(0, 4, "FAIL"), 0);
}

INSTANTIATE_TEST_SUITE_P(
    FastbootSetActiveUserErrorTests, FastbootSetActiveUserErrorTest,
    testing::ValuesIn<FastbootSetActiveUserErrorTest::ParamType>({
        {"no_name", ""},
        {"no_such_name", ":squid"},
        {"cant_set_r", ":r"},
    }),
    [](testing::TestParamInfo<FastbootSetActiveUserErrorTest::ParamType> const& info) {
      return info.param.name;
    });

TEST_F(FastbootSlotTest, SetActiveSlotWriteFailure) {
  // Do NOT call InitializeAbr in order to simulate
  // a write error due to a missing partition.

  ZirconBootOps zb_ops = mock_zb_ops().GetZirconBootOps();

  Fastboot fastboot(download_buffer, zb_ops);
  fastboot::TestTransport transport;

  transport.AddInPacket(std::string("set_active:b"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_error());
  auto sent_packets = transport.GetOutPackets();
  ASSERT_EQ(sent_packets.size(), 1ULL);
  ASSERT_EQ(sent_packets[0].compare(0, 4, "FAIL"), 0);
}

struct VarErrorTestCase {
  char const* test_name;
  char const* var;
};

using VarErrorTest = ::testing::TestWithParam<VarErrorTestCase>;

TEST_P(VarErrorTest, TestVarError) {
  VarErrorTestCase const& test_case = GetParam();

  MockZirconBootOps mock_zb_ops;
  Fastboot fastboot(download_buffer, mock_zb_ops.GetZirconBootOps());
  fastboot::TestTransport transport;
  std::string command = std::string{"getvar:"} + test_case.var;

  transport.AddInPacket(command);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  auto sent_packets = transport.GetOutPackets();
  ASSERT_EQ(sent_packets.size(), 1ULL);
  ASSERT_EQ(sent_packets[0].compare(0, 4, "FAIL"), 0);
}

INSTANTIATE_TEST_SUITE_P(VarErrorTests, VarErrorTest,
                         testing::ValuesIn<VarErrorTest::ParamType>({
                             // slot-retry-count
                             {"slot_retry_count_too_few_args", "slot-retry-count"},
                             {"slot_retry_count_invalid_name", "slot-retry-count:r"},
                             {"slot_retry_count_read_error", "slot-retry-count:a"},
                             // slot-successful
                             {"slot_successful_too_few_args", "slot-successful"},
                             {"slot_successful_invalid_name", "slot-successful:squid"},
                             {"slot_successful_read_error", "slot-successful:a"},
                             // slot-last-set-actvie
                             {"slot_last_set_active_read_error", "slot-last-set-active"},
                             // slot-unbootable
                             {"slot_unbootable_too_few_args", "slot-unbootable"},
                             {"slot_unbootable_invalid_name", "slot-unbootable:squid"},
                             {"slot_unbootable_read_error", "slot-unbootable:a"},
                             // nonexistent variable
                             {"nonexistent_variable", "non-existing"},
                             // too few args
                             {"no_var", ""},
                         }),
                         [](testing::TestParamInfo<VarErrorTest::ParamType> const& info) {
                           return info.param.test_name;
                         });

TEST_F(FastbootFlashTest, FlashPartition) {
  constexpr size_t partition_size = 0x100;

  mock_zb_ops().AddPartition(GPT_ZIRCON_A_NAME, sizeof(uint8_t) * partition_size);
  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());

  // Download some data to flash to the partition.
  std::vector<uint8_t> download_content;
  for (size_t i = 0; i < partition_size; i++) {
    download_content.push_back(static_cast<uint8_t>(i));
  }
  ASSERT_NO_FATAL_FAILURE(DownloadData(fastboot, download_content));

  fastboot::TestTransport transport;

  transport.AddInPacket(std::string("flash:") + GPT_ZIRCON_A_NAME);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());

  fastboot::Packets expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));

  uint8_t read_buf[partition_size] = {0};
  ret = mock_zb_ops().ReadFromPartition(GPT_ZIRCON_A_NAME, 0, sizeof(read_buf), read_buf);
  ASSERT_TRUE(ret.is_ok());
  ASSERT_TRUE(memcmp(read_buf, download_content.data(), download_content.size()) == 0);
}

TEST_F(FastbootFlashTest, FlashPartitionFailedToWritePartition) {
  // Do NOT add any partitions. Write should fail.
  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());

  // Download some data to flash to the partition.
  std::vector<uint8_t> download_content(128, 0);
  ASSERT_NO_FATAL_FAILURE(DownloadData(fastboot, download_content));

  fastboot::TestTransport transport;

  transport.AddInPacket(std::string("flash:") + GPT_ZIRCON_A_NAME);
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_error());

  // Should fail while searching for gpt device.
  auto sent_packets = transport.GetOutPackets();
  ASSERT_EQ(sent_packets.size(), 1ULL);
  ASSERT_EQ(sent_packets[0].compare(0, 4, "FAIL"), 0);
}

// TODO(b/235489025): Extends `StubBootServices` to cover the mock of efi_runtime_services.
EFIAPI efi_status ResetSystemSucceed(efi_reset_type, efi_status, size_t, void*) {
  return EFI_SUCCESS;
}

TEST_F(FastbootFlashTest, RebootNormal) {
  auto cleanup = SetupEfiGlobalState(stub_service(), image_device());
  efi_runtime_services runtime_services{
      .ResetSystem = ResetSystemSucceed,
  };
  gEfiSystemTable->RuntimeServices = &runtime_services;

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  // Set to a different initial boot mode.
  FakeRebootMode bootmode(RebootMode::kBootloader);

  transport.AddInPacket(std::string("reboot"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  std::vector<std::string> expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));

  ASSERT_EQ(test_reboot_mode, RebootMode::kNormal);
}

TEST_F(FastbootFlashTest, RebootBootloader) {
  auto cleanup = SetupEfiGlobalState(stub_service(), image_device());
  efi_runtime_services runtime_services{
      .ResetSystem = ResetSystemSucceed,
  };
  gEfiSystemTable->RuntimeServices = &runtime_services;

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  // Set to a different initial boot mode.
  FakeRebootMode bootmode(RebootMode::kNormal);

  transport.AddInPacket(std::string("reboot-bootloader"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  std::vector<std::string> expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));

  ASSERT_EQ(test_reboot_mode, RebootMode::kBootloader);
}

TEST_F(FastbootFlashTest, RebootRecovery) {
  auto cleanup = SetupEfiGlobalState(stub_service(), image_device());
  efi_runtime_services runtime_services{
      .ResetSystem = ResetSystemSucceed,
  };
  gEfiSystemTable->RuntimeServices = &runtime_services;

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  // Set to a different initial boot mode.
  FakeRebootMode bootmode(RebootMode::kNormal);

  transport.AddInPacket(std::string("reboot-recovery"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_ok());
  std::vector<std::string> expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));

  ASSERT_EQ(test_reboot_mode, RebootMode::kRecovery);
}

TEST_F(FastbootFlashTest, RebootSetRebootModeFail) {
  auto cleanup = SetupEfiGlobalState(stub_service(), image_device());
  efi_runtime_services runtime_services{
      .ResetSystem = ResetSystemSucceed,
  };
  gEfiSystemTable->RuntimeServices = &runtime_services;

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  // Set returned value to false
  FakeRebootMode bootmode(RebootMode::kNormal, false);

  transport.AddInPacket(std::string("reboot"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_error());

  auto sent_packets = transport.GetOutPackets();
  ASSERT_EQ(sent_packets.size(), 1ULL);
  ASSERT_EQ(sent_packets[0].compare(0, 4, "FAIL"), 0);
}

EFIAPI efi_status ResetSystemFailed(efi_reset_type, efi_status, size_t, void*) {
  return EFI_ABORTED;
}

TEST_F(FastbootFlashTest, RebootResetSystemFail) {
  auto cleanup = SetupEfiGlobalState(stub_service(), image_device());
  efi_runtime_services runtime_services{
      .ResetSystem = ResetSystemFailed,
  };
  gEfiSystemTable->RuntimeServices = &runtime_services;

  Fastboot fastboot(download_buffer, mock_zb_ops().GetZirconBootOps());
  fastboot::TestTransport transport;

  transport.AddInPacket(std::string("reboot"));
  zx::status ret = fastboot.ProcessPacket(&transport);
  ASSERT_TRUE(ret.is_error());

  // We should still receive an OKAY packet.
  std::vector<std::string> expected_packets = {"OKAY"};
  ASSERT_NO_FATAL_FAILURE(CheckPacketsEqual(transport.GetOutPackets(), expected_packets));
}

}  // namespace

}  // namespace gigaboot
