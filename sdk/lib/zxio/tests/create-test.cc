// Copyright 2021 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <fidl/fuchsia.io/cpp/wire_test_base.h>
#include <lib/async-loop/cpp/loop.h>
#include <lib/async-loop/default.h>
#include <lib/zx/channel.h>
#include <lib/zx/event.h>
#include <lib/zxio/cpp/create_with_type.h>
#include <lib/zxio/zxio.h>
#include <zircon/syscalls/types.h>
#include <zircon/types.h>

#include <zxtest/zxtest.h>

#include "sdk/lib/zxio/tests/test_file_server_base.h"
#include "sdk/lib/zxio/tests/test_node_server.h"

TEST(Create, InvalidArgs) {
  ASSERT_STATUS(zxio_create(ZX_HANDLE_INVALID, nullptr), ZX_ERR_INVALID_ARGS);

  zxio_storage_t storage;
  ASSERT_STATUS(zxio_create(ZX_HANDLE_INVALID, &storage), ZX_ERR_INVALID_ARGS);

  zx::channel channel0, channel1;
  ASSERT_OK(zx::channel::create(0u, &channel0, &channel1));
  ASSERT_STATUS(zxio_create(channel0.release(), nullptr), ZX_ERR_INVALID_ARGS);

  // Make sure that the handle is closed.
  zx_signals_t pending = 0;
  ASSERT_STATUS(channel1.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
  EXPECT_EQ(pending & ZX_CHANNEL_PEER_CLOSED, ZX_CHANNEL_PEER_CLOSED);
}

TEST(Create, NotSupported) {
  zx::event event;
  ASSERT_OK(zx::event::create(0u, &event));
  zxio_storage_t storage;
  ASSERT_STATUS(zxio_create(event.release(), &storage), ZX_ERR_NOT_SUPPORTED);
  zxio_t* io = &storage.io;
  zx::handle handle;
  ASSERT_OK(zxio_release(io, handle.reset_and_get_address()));
  ASSERT_OK(zxio_close(io));

  zx_info_handle_basic_t info = {};
  ASSERT_OK(handle.get_info(ZX_INFO_HANDLE_BASIC, &info, sizeof(info), nullptr, nullptr));
  EXPECT_EQ(info.type, ZX_OBJ_TYPE_EVENT);
}

class CreateWithTypeBaseTest : public zxtest::Test {
 public:
  void SetUp() override { ASSERT_OK(zx::channel::create(0, &channel0_, &channel1_)); }

  zxio_storage_t& storage() { return storage_; }
  zx::channel& channel() { return channel0_; }

  void AssertChannelClosed() {
    zx_signals_t pending = 0;
    ASSERT_STATUS(channel1_.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
    EXPECT_EQ(pending & ZX_CHANNEL_PEER_CLOSED, ZX_CHANNEL_PEER_CLOSED);
  }

 private:
  zxio_storage_t storage_;
  zx::channel channel0_, channel1_;
};

class CreateWithTypeBaseWithEventTest : public CreateWithTypeBaseTest {
 public:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(CreateWithTypeBaseTest::SetUp());
    ASSERT_OK(zx::eventpair::create(0, &event0_, &event1_));
  }

  zx::eventpair& event() { return event0_; }

  void AssertEventClosed() {
    zx_signals_t pending = 0;
    ASSERT_STATUS(event1_.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
    EXPECT_EQ(pending & ZX_EVENTPAIR_PEER_CLOSED, ZX_EVENTPAIR_PEER_CLOSED);
  }

 private:
  zx::eventpair event0_, event1_;
};

class CreateWithTypeSynchronousDatagramSocketTest : public CreateWithTypeBaseWithEventTest {};

// Tests that calling zxio_create_with_type() with an invalid storage pointer
// still closes all the handles known for the type.
TEST_F(CreateWithTypeSynchronousDatagramSocketTest, InvalidStorage) {
  zx_handle_t event_handle = event().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_SYNCHRONOUS_DATAGRAM_SOCKET,
                                      event_handle, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertEventClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeSynchronousDatagramSocketTest, InvalidEvent) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_SYNCHRONOUS_DATAGRAM_SOCKET,
                                      ZX_HANDLE_INVALID, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeSynchronousDatagramSocketTest, InvalidChannel) {
  zx_handle_t event_handle = event().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_SYNCHRONOUS_DATAGRAM_SOCKET,
                                      event_handle, ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
  AssertEventClosed();
}

class CreateWithTypeDirectoryTest : public CreateWithTypeBaseWithEventTest {};

TEST_F(CreateWithTypeDirectoryTest, InvalidStorage) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_DIR, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeDirectoryTest, InvalidChannel) {
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_DIR, ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
}

class CreateWithTypeNodeTest : public CreateWithTypeBaseWithEventTest {};

TEST_F(CreateWithTypeNodeTest, InvalidStorage) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_NODE, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeNodeTest, InvalidChannel) {
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_NODE, ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
}

class CreateWithTypeBaseWithSocketTest : public CreateWithTypeBaseTest {
 public:
  void SetUp() override {
    ASSERT_NO_FATAL_FAILURE(CreateWithTypeBaseTest::SetUp());
    ASSERT_OK(zx::socket::create(ZX_SOCKET_STREAM, &socket0_, &socket1_));
  }

  zx::socket& socket() { return socket0_; }
  const zx_info_socket_t& info() const { return info_; }

  void AssertSocketClosed() {
    zx_signals_t pending = 0;
    ASSERT_STATUS(socket1_.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
    EXPECT_EQ(pending & ZX_SOCKET_PEER_CLOSED, ZX_SOCKET_PEER_CLOSED);
  }

 private:
  zx::socket socket0_, socket1_;
  zx_info_socket_t info_;
};

class CreateWithTypeStreamSocketTest : public CreateWithTypeBaseWithSocketTest {};

TEST_F(CreateWithTypeStreamSocketTest, InvalidStorage) {
  zx_handle_t socket_handle = socket().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_STREAM_SOCKET, socket_handle,
                                      &info(), /*is_connected=*/false, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeStreamSocketTest, InvalidSocket) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_STREAM_SOCKET, ZX_HANDLE_INVALID,
                                      &info(), /*is_connected=*/false, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeStreamSocketTest, InvalidChannel) {
  zx_handle_t socket_handle = socket().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_STREAM_SOCKET, socket_handle,
                                      &info(), /*is_connected=*/false, ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
}

TEST_F(CreateWithTypeStreamSocketTest, InvalidInfo) {
  zx_handle_t socket_handle = socket().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_STREAM_SOCKET, socket_handle,
                                      nullptr, /*is_connected=*/false, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
  AssertChannelClosed();
}

class CreateWithTypeDatagramSocketTest : public CreateWithTypeBaseWithSocketTest {
 public:
  const zxio_datagram_prelude_size_t& prelude_size() const { return prelude_size_; }

 private:
  zxio_datagram_prelude_size_t prelude_size_{};
};

TEST_F(CreateWithTypeDatagramSocketTest, InvalidStorage) {
  zx_handle_t socket_handle = socket().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_DATAGRAM_SOCKET, socket_handle,
                                      &info(), &prelude_size(), channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeDatagramSocketTest, InvalidSocket) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_DATAGRAM_SOCKET,
                                      ZX_HANDLE_INVALID, &info(), &prelude_size(), channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeDatagramSocketTest, InvalidInfo) {
  zx_handle_t socket_handle = socket().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_DATAGRAM_SOCKET, socket_handle,
                                      nullptr, &prelude_size(), channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeDatagramSocketTest, PreludeSize) {
  zx_handle_t socket_handle = socket().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_DATAGRAM_SOCKET, socket_handle,
                                      &info(), nullptr, channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeDatagramSocketTest, InvalidChannel) {
  zx_handle_t socket_handle = socket().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_DATAGRAM_SOCKET, socket_handle,
                                      &info(), &prelude_size(), ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
}

class CreateWithTypePipeTest : public CreateWithTypeBaseWithSocketTest {};

TEST_F(CreateWithTypePipeTest, InvalidStorage) {
  zx_handle_t socket_handle = socket().release();
  ASSERT_STATUS(zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_PIPE, socket_handle, &info()),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
}

TEST_F(CreateWithTypePipeTest, InvalidSocket) {
  ASSERT_STATUS(
      zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_PIPE, ZX_HANDLE_INVALID, &info()),
      ZX_ERR_INVALID_ARGS);
}

TEST_F(CreateWithTypePipeTest, InvalidInfo) {
  zx_handle_t socket_handle = socket().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_PIPE, socket_handle, nullptr),
                ZX_ERR_INVALID_ARGS);
  AssertSocketClosed();
}

class CreateWithTypeRawSocketTest : public CreateWithTypeBaseWithEventTest {};

TEST_F(CreateWithTypeRawSocketTest, InvalidStorage) {
  zx_handle_t event_handle = event().release();
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(
      zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_RAW_SOCKET, event_handle, channel_handle),
      ZX_ERR_INVALID_ARGS);
  AssertEventClosed();
  AssertChannelClosed();
}

TEST_F(CreateWithTypeRawSocketTest, InvalidEvent) {
  zx_handle_t channel_handle = channel().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_RAW_SOCKET, ZX_HANDLE_INVALID,
                                      channel_handle),
                ZX_ERR_INVALID_ARGS);
  AssertChannelClosed();
}

TEST_F(CreateWithTypeRawSocketTest, InvalidChannel) {
  zx_handle_t event_handle = event().release();
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_RAW_SOCKET, event_handle,
                                      ZX_HANDLE_INVALID),
                ZX_ERR_INVALID_ARGS);
  AssertEventClosed();
}

class CreateWithTypeVmoTest : public CreateWithTypeBaseTest {
 public:
  void SetUp() override {
    ASSERT_OK(zx::vmo::create(4096, 0, &parent_));
    ASSERT_OK(parent_.create_child(ZX_VMO_CHILD_SLICE, 0, 4096, &child_));
    ASSERT_OK(zx::stream::create(0, child_, 0, &stream_));
  }

  zx::vmo& child() { return child_; }
  zx::stream& stream() { return stream_; }

  void AssertParentVmoHasNoChildren() {
    zx_signals_t pending = 0;
    ASSERT_STATUS(parent_.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
    EXPECT_EQ(pending & ZX_VMO_ZERO_CHILDREN, ZX_VMO_ZERO_CHILDREN);
  }

 private:
  zx::vmo parent_, child_;
  zx::stream stream_;
};

TEST_F(CreateWithTypeVmoTest, InvalidStorage) {
  zx_handle_t child_handle = child().release();
  ASSERT_STATUS(
      zxio_create_with_type(nullptr, ZXIO_OBJECT_TYPE_VMO, child_handle, stream().release()),
      ZX_ERR_INVALID_ARGS);
  AssertParentVmoHasNoChildren();
}

TEST_F(CreateWithTypeVmoTest, InvalidVmo) {
  ASSERT_STATUS(zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_VMO, ZX_HANDLE_INVALID,
                                      stream().release()),
                ZX_ERR_INVALID_ARGS);
}

TEST_F(CreateWithTypeVmoTest, InvalidStream) {
  zx_handle_t child_handle = child().release();
  ASSERT_STATUS(
      zxio_create_with_type(&storage(), ZXIO_OBJECT_TYPE_VMO, child_handle, ZX_HANDLE_INVALID),
      ZX_ERR_INVALID_ARGS);
  // The stream is considered a child of the VMO from which it was created.
  // Closing the stream should leave the parent with no children.
  stream().reset();
  AssertParentVmoHasNoChildren();
}

template <typename NodeServer>
class CreateTestBase : public zxtest::Test {
 protected:
  CreateTestBase() : control_loop_(&kAsyncLoopConfigNoAttachToCurrentThread) {}

  using ProtocolType = typename NodeServer::_EnclosingProtocol;

  void SetUp() final {
    zx::status node_ends = fidl::CreateEndpoints<ProtocolType>();
    ASSERT_OK(node_ends.status_value());
    node_client_end_ = std::move(node_ends->client);
    node_server_end_ = std::move(node_ends->server);
  }

  void TearDown() final { control_loop_.Shutdown(); }

  zx::channel TakeClientChannel() { return node_client_end_.TakeChannel(); }

  void StartServerThread() {
    fidl::BindServer(control_loop_.dispatcher(), std::move(node_server_end_), &node_server_);
    control_loop_.StartThread("control");
  }

  zxio_t* zxio() { return &storage_.io; }
  zxio_storage_t* storage() { return &storage_; }

  void SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated node_info) {
    ASSERT_OK(fidl::WireSendEvent(node_server_end_)->OnOpen(ZX_OK, std::move(node_info)));
  }

  NodeServer& node_server() { return node_server_; }

 private:
  zxio_storage_t storage_;
  fidl::ClientEnd<ProtocolType> node_client_end_;
  fidl::ServerEnd<ProtocolType> node_server_end_;
  NodeServer node_server_;
  async::Loop control_loop_;
};

using CreateTest = CreateTestBase<zxio_tests::DescribeNodeServer>;
using CreateWithOnOpenTest = CreateTestBase<zxio_tests::CloseOnlyNodeServer>;

using DescribeCompleter = fidl::WireServer<::fuchsia_io::Node>::DescribeDeprecatedCompleter;

class SyncNodeServer : public zxio_tests::DescribeNodeServer {
 public:
  void Sync(SyncCompleter::Sync& completer) final { completer.ReplySuccess(); }
};

using CreateDirectoryTest = CreateTestBase<SyncNodeServer>;

TEST_F(CreateDirectoryTest, Directory) {
  node_server().set_describe_function([](DescribeCompleter::Sync& completer) mutable {
    completer.Reply(fuchsia_io::wire::NodeInfoDeprecated::WithDirectory({}));
  });

  StartServerThread();

  ASSERT_OK(zxio_create(TakeClientChannel().release(), storage()));

  EXPECT_OK(zxio_sync(zxio()));

  ASSERT_OK(zxio_close(zxio()));
}

TEST_F(CreateDirectoryTest, DirectoryWithType) {
  StartServerThread();

  ASSERT_OK(zxio_create_with_type(storage(), ZXIO_OBJECT_TYPE_DIR, TakeClientChannel().release()));

  EXPECT_OK(zxio_sync(zxio()));

  ASSERT_OK(zxio_close(zxio()));
}

TEST_F(CreateDirectoryTest, DirectoryWithTypeWrapper) {
  StartServerThread();

  ASSERT_OK(zxio::CreateDirectory(storage(),
                                  fidl::ClientEnd<fuchsia_io::Directory>(TakeClientChannel())));

  EXPECT_OK(zxio_sync(zxio()));

  ASSERT_OK(zxio_close(zxio()));
}

using CreateDirectoryWithOnOpenTest = CreateTestBase<SyncNodeServer>;

TEST_F(CreateDirectoryWithOnOpenTest, Directory) {
  SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated::WithDirectory({}));

  ASSERT_OK(zxio_create_with_on_open(TakeClientChannel().release(), storage()));

  StartServerThread();

  EXPECT_OK(zxio_sync(zxio()));

  ASSERT_OK(zxio_close(zxio()));
}

class TestFileServerWithDescribe : public zxio_tests::TestReadFileServer {
 public:
  void set_file(fuchsia_io::wire::FileObject file) { file_ = std::move(file); }

 protected:
  void DescribeDeprecated(DescribeDeprecatedCompleter::Sync& completer) final {
    completer.Reply(fuchsia_io::wire::NodeInfoDeprecated::WithFile(
        fidl::ObjectView<decltype(file_)>::FromExternal(&file_)));
  }

 private:
  fuchsia_io::wire::FileObject file_;
};

using CreateFileTest = CreateTestBase<TestFileServerWithDescribe>;

TEST_F(CreateFileTest, File) {
  zx::event file_event;
  ASSERT_OK(zx::event::create(0u, &file_event));
  zx::stream stream;

  node_server().set_file({
      .event = std::move(file_event),
      .stream = std::move(stream),
  });

  StartServerThread();

  ASSERT_OK(zxio_create(TakeClientChannel().release(), storage()));

  // Sanity check the zxio by reading some test data from the server.
  char buffer[sizeof(zxio_tests::TestReadFileServer::kTestData)];
  size_t actual = 0u;

  ASSERT_OK(zxio_read(zxio(), buffer, sizeof(buffer), 0u, &actual));

  EXPECT_EQ(sizeof(buffer), actual);
  EXPECT_BYTES_EQ(buffer, zxio_tests::TestReadFileServer::kTestData, sizeof(buffer));
}

using CreateFileWithOnOpenTest = CreateTestBase<zxio_tests::TestReadFileServer>;

TEST_F(CreateFileWithOnOpenTest, File) {
  zx::event file_event;
  ASSERT_OK(zx::event::create(0u, &file_event));
  zx::stream stream;

  fuchsia_io::wire::FileObject file = {
      .event = std::move(file_event),
      .stream = std::move(stream),
  };
  SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated::WithFile(
      fidl::ObjectView<decltype(file)>::FromExternal(&file)));

  ASSERT_OK(zxio_create_with_on_open(TakeClientChannel().release(), storage()));

  StartServerThread();

  // Sanity check the zxio by reading some test data from the server.
  char buffer[sizeof(zxio_tests::TestReadFileServer::kTestData)];
  size_t actual = 0u;

  ASSERT_OK(zxio_read(zxio(), buffer, sizeof(buffer), 0u, &actual));

  EXPECT_EQ(sizeof(buffer), actual);
  EXPECT_BYTES_EQ(buffer, zxio_tests::TestReadFileServer::kTestData, sizeof(buffer));

  ASSERT_OK(zxio_close(zxio()));
}

TEST(CreateWithTypeTest, Pipe) {
  zx::socket socket0, socket1;
  ASSERT_OK(zx::socket::create(0u, &socket0, &socket1));

  zx_info_socket_t info = {};
  ASSERT_OK(socket0.get_info(ZX_INFO_SOCKET, &info, sizeof(info), nullptr, nullptr));

  zxio_storage_t storage;
  ASSERT_OK(zxio_create_with_type(&storage, ZXIO_OBJECT_TYPE_PIPE, socket0.release(), &info));
  zxio_t* zxio = &storage.io;

  // Send some data through the kernel socket object and read it through zxio to
  // sanity check that the pipe is functional.
  int32_t data = 0x1a2a3a4a;
  size_t actual = 0u;
  ASSERT_OK(socket1.write(0u, &data, sizeof(data), &actual));
  EXPECT_EQ(actual, sizeof(data));

  int32_t buffer = 0;
  ASSERT_OK(zxio_read(zxio, &buffer, sizeof(buffer), 0u, &actual));
  EXPECT_EQ(actual, sizeof(buffer));
  EXPECT_EQ(buffer, data);

  ASSERT_OK(zxio_close(zxio));
}

TEST(CreateWithTypeWrapperTest, Pipe) {
  zx::socket socket0, socket1;
  ASSERT_OK(zx::socket::create(0u, &socket0, &socket1));

  zx_info_socket_t info = {};
  ASSERT_OK(socket0.get_info(ZX_INFO_SOCKET, &info, sizeof(info), nullptr, nullptr));

  zxio_storage_t storage;
  ASSERT_OK(zxio::CreatePipe(&storage, std::move(socket0), info));
  zxio_t* zxio = &storage.io;

  // Send some data through the kernel socket object and read it through zxio to
  // sanity check that the pipe is functional.
  int32_t data = 0x1a2a3a4a;
  size_t actual = 0u;
  ASSERT_OK(socket1.write(0u, &data, sizeof(data), &actual));
  EXPECT_EQ(actual, sizeof(data));

  int32_t buffer = 0;
  ASSERT_OK(zxio_read(zxio, &buffer, sizeof(buffer), 0u, &actual));
  EXPECT_EQ(actual, sizeof(buffer));
  EXPECT_EQ(buffer, data);

  ASSERT_OK(zxio_close(zxio));
}

TEST_F(CreateTest, Service) {
  node_server().set_describe_function([](DescribeCompleter::Sync& completer) mutable {
    completer.Reply(fuchsia_io::wire::NodeInfoDeprecated::WithService({}));
  });

  StartServerThread();

  ASSERT_OK(zxio_create(TakeClientChannel().release(), storage()));

  ASSERT_OK(zxio_close(zxio()));
}

TEST_F(CreateWithOnOpenTest, Service) {
  SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated::WithService({}));

  ASSERT_OK(zxio_create_with_on_open(TakeClientChannel().release(), storage()));

  StartServerThread();

  ASSERT_OK(zxio_close(zxio()));
}

TEST_F(CreateTest, Tty) {
  zx::eventpair event0, event1;
  ASSERT_OK(zx::eventpair::create(0, &event0, &event1));
  node_server().set_describe_function(
      [event1 = std::move(event1)](DescribeCompleter::Sync& completer) mutable {
        completer.Reply(fuchsia_io::wire::NodeInfoDeprecated::WithTty({
            .event = std::move(event1),
        }));
      });

  StartServerThread();

  ASSERT_OK(zxio_create(TakeClientChannel().release(), storage()));

  // Closing the zxio object should close our eventpair's peer event.
  zx_signals_t pending = 0;
  ASSERT_STATUS(event0.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
  EXPECT_NE(pending & ZX_EVENTPAIR_PEER_CLOSED, ZX_EVENTPAIR_PEER_CLOSED, "pending is %u", pending);

  ASSERT_OK(zxio_close(zxio()));

  ASSERT_STATUS(event0.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
  EXPECT_EQ(pending & ZX_EVENTPAIR_PEER_CLOSED, ZX_EVENTPAIR_PEER_CLOSED, "pending is %u", pending);
}

TEST_F(CreateWithOnOpenTest, Tty) {
  zx::eventpair event0, event1;

  ASSERT_OK(zx::eventpair::create(0, &event0, &event1));
  SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated::WithTty({
      .event = std::move(event1),
  }));

  ASSERT_OK(zxio_create_with_on_open(TakeClientChannel().release(), storage()));

  // Closing the zxio object should close our eventpair's peer event.
  zx_signals_t pending = 0;
  ASSERT_STATUS(event0.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
  EXPECT_NE(pending & ZX_EVENTPAIR_PEER_CLOSED, ZX_EVENTPAIR_PEER_CLOSED, "pending is %u", pending);

  StartServerThread();

  ASSERT_OK(zxio_close(zxio()));

  ASSERT_STATUS(event0.wait_one(0u, zx::time::infinite_past(), &pending), ZX_ERR_TIMED_OUT);
  EXPECT_EQ(pending & ZX_EVENTPAIR_PEER_CLOSED, ZX_EVENTPAIR_PEER_CLOSED, "pending is %u", pending);
}

class TestVmofileServerWithDescribe : public zxio_tests::TestVmofileServer {
 public:
  void set_vmofile(fuchsia_io::wire::VmofileDeprecated vmofile) { vmofile_ = std::move(vmofile); }

 protected:
  void DescribeDeprecated(DescribeDeprecatedCompleter::Sync& completer) final {
    completer.Reply(fuchsia_io::wire::NodeInfoDeprecated::WithVmofileDeprecated(
        fidl::ObjectView<decltype(vmofile_)>::FromExternal(&vmofile_)));
  }

 private:
  fuchsia_io::wire::VmofileDeprecated vmofile_;
};

using CreateVmofileTest = CreateTestBase<TestVmofileServerWithDescribe>;

TEST_F(CreateVmofileTest, File) {
  const uint64_t vmo_size = 5678;
  const uint64_t file_start_offset = 1234;
  const uint64_t file_length = 345;
  const uint64_t offset_within_file = 234;

  node_server().set_seek_offset(offset_within_file);

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(vmo_size, 0u, &vmo));

  node_server().set_vmofile({
      .vmo = std::move(vmo),
      .offset = file_start_offset,
      .length = file_length,
  });

  StartServerThread();

  ASSERT_OK(zxio_create(TakeClientChannel().release(), storage()));

  ASSERT_OK(zxio_close(zxio()));
}

TEST(CreateVmofileWithTypeTest, File) {
  const uint64_t vmo_size = 5678;
  const uint64_t file_start_offset = 1234;

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(vmo_size, 0u, &vmo));

  zx::stream stream;
  ASSERT_OK(zx::stream::create(ZX_STREAM_MODE_READ, vmo, file_start_offset, &stream));

  zxio_storage_t storage;
  ASSERT_OK(zxio_create_with_type(&storage, ZXIO_OBJECT_TYPE_VMO, vmo.release(), stream.release()));
  zxio_t* zxio = &storage.io;

  zxio_node_attributes_t attr;
  EXPECT_OK(zxio_attr_get(zxio, &attr));

  EXPECT_TRUE(attr.has.content_size);
  EXPECT_EQ(attr.content_size, vmo_size);

  size_t seek_current_offset = 0u;
  EXPECT_OK(zxio_seek(zxio, ZXIO_SEEK_ORIGIN_CURRENT, 0, &seek_current_offset));
  EXPECT_EQ(static_cast<size_t>(file_start_offset), seek_current_offset);

  ASSERT_OK(zxio_close(zxio));
}

TEST(CreateVmofileWithTypeWrapperTest, File) {
  const uint64_t vmo_size = 5678;
  const uint64_t file_start_offset = 1234;

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(vmo_size, 0u, &vmo));

  zx::stream stream;
  ASSERT_OK(zx::stream::create(ZX_STREAM_MODE_READ, vmo, file_start_offset, &stream));

  zxio_storage_t storage;
  ASSERT_OK(zxio::CreateVmo(&storage, std::move(vmo), std::move(stream)));
  zxio_t* zxio = &storage.io;

  zxio_node_attributes_t attr;
  EXPECT_OK(zxio_attr_get(zxio, &attr));

  EXPECT_TRUE(attr.has.content_size);
  EXPECT_EQ(attr.content_size, vmo_size);

  size_t seek_current_offset = 0u;
  EXPECT_OK(zxio_seek(zxio, ZXIO_SEEK_ORIGIN_CURRENT, 0, &seek_current_offset));
  EXPECT_EQ(static_cast<size_t>(file_start_offset), seek_current_offset);

  ASSERT_OK(zxio_close(zxio));
}

using CreateVmofileWithOnOpenTest = CreateTestBase<zxio_tests::TestVmofileServer>;

TEST_F(CreateVmofileWithOnOpenTest, File) {
  const uint64_t vmo_size = 5678;
  const uint64_t file_start_offset = 1234;
  const uint64_t file_length = 345;
  const uint64_t offset_within_file = 234;

  zx::vmo vmo;
  ASSERT_OK(zx::vmo::create(vmo_size, 0u, &vmo));

  fuchsia_io::wire::VmofileDeprecated vmofile = {
      .vmo = std::move(vmo),
      .offset = file_start_offset,
      .length = file_length,
  };
  SendOnOpenEvent(fuchsia_io::wire::NodeInfoDeprecated::WithVmofileDeprecated(
      fidl::ObjectView<decltype(vmofile)>::FromExternal(&vmofile)));

  node_server().set_seek_offset(offset_within_file);

  StartServerThread();

  ASSERT_OK(zxio_create_with_on_open(TakeClientChannel().release(), storage()));

  // Sanity check the zxio object.
  zxio_node_attributes_t attr;
  EXPECT_OK(zxio_attr_get(zxio(), &attr));

  EXPECT_TRUE(attr.has.content_size);
  EXPECT_EQ(attr.content_size, file_length);

  size_t seek_current_offset = 0u;
  EXPECT_OK(zxio_seek(zxio(), ZXIO_SEEK_ORIGIN_CURRENT, 0, &seek_current_offset));
  EXPECT_EQ(static_cast<size_t>(offset_within_file), seek_current_offset);

  size_t seek_start_offset = 0u;
  EXPECT_OK(zxio_seek(zxio(), ZXIO_SEEK_ORIGIN_START, 0, &seek_start_offset));
  EXPECT_EQ(0, seek_start_offset);

  size_t seek_end_offset = 0u;
  EXPECT_OK(zxio_seek(zxio(), ZXIO_SEEK_ORIGIN_END, 0, &seek_end_offset));
  EXPECT_EQ(static_cast<size_t>(file_length), seek_end_offset);

  ASSERT_OK(zxio_close(zxio()));
}
