// Copyright 2022 The Fuchsia Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "src/media/audio/services/mixer/fidl/graph_server.h"

#include <lib/syslog/cpp/macros.h>

#include <gtest/gtest.h>

#include "fidl/fuchsia.audio.effects/cpp/markers.h"
#include "fidl/fuchsia.audio.effects/cpp/wire_types.h"
#include "fidl/fuchsia.audio.mixer/cpp/natural_types.h"
#include "fidl/fuchsia.mediastreams/cpp/wire_types.h"
#include "fidl/fuchsia.mem/cpp/wire_types.h"
#include "lib/fidl/cpp/wire/arena.h"
#include "src/media/audio/lib/clock/testing/clock_test.h"
#include "src/media/audio/services/common/logging.h"
#include "src/media/audio/services/common/testing/test_server_and_client.h"
#include "src/media/audio/services/mixer/common/basic_types.h"
#include "src/media/audio/services/mixer/fidl/clock_registry.h"
#include "src/media/audio/services/mixer/fidl/real_clock_factory.h"
#include "src/media/audio/services/mixer/fidl/synthetic_clock_server.h"

namespace media_audio {
namespace {

using ::fuchsia_audio_mixer::CreateNodeError;

const Format kFormat = Format::CreateOrDie({
    .sample_type = ::fuchsia_audio::SampleType::kFloat32,
    .channels = 2,
    .frames_per_second = 48000,
});

fuchsia_audio::wire::Format MakeInvalidFormatFidl(fidl::AnyArena& arena) {
  return fuchsia_audio::wire::Format::Builder(arena)
      .sample_type(fuchsia_audio::SampleType::kFloat32)
      .channel_count(0)  // illegal
      .frames_per_second(48000)
      .Build();
}

zx::clock MakeClock() {
  zx::clock clock;
  if (auto status = zx::clock::create(
          ZX_CLOCK_OPT_AUTO_START | ZX_CLOCK_OPT_MONOTONIC | ZX_CLOCK_OPT_CONTINUOUS, nullptr,
          &clock);
      status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "clock.create failed";
  }
  return clock;
}

fuchsia_audio_mixer::wire::ReferenceClock MakeReferenceClock(fidl::AnyArena& arena) {
  return fuchsia_audio_mixer::wire::ReferenceClock::Builder(arena)
      .handle(MakeClock())
      .domain(Clock::kMonotonicDomain)
      .Build();
}

zx::vmo MakeVmo(size_t size = 1024) {
  zx::vmo vmo;
  if (auto status = zx::vmo::create(size, 0, &vmo); status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "zx::vmo::create failed";
  }
  return vmo;
}

zx::vmo MakeInvalidVmo(size_t size = 1024) {
  zx::vmo vmo;
  if (auto status = zx::vmo::create(size, 0, &vmo); status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "zx::vmo::create failed";
  }
  // Remove ZX_RIGHT_MAP.
  if (auto status = vmo.replace(
          ZX_RIGHT_READ | ZX_RIGHT_WRITE | ZX_RIGHT_TRANSFER | ZX_RIGHT_GET_PROPERTY, &vmo);
      status != ZX_OK) {
    FX_PLOGS(FATAL, status) << "zx::vmo::replace failed";
  }
  return vmo;
}

fidl::WireTableBuilder<fuchsia_audio_mixer::wire::StreamSinkProducer> MakeDefaultStreamSinkProducer(
    fidl::AnyArena& arena) {
  auto [stream_sink_client, stream_sink_server] = CreateClientOrDie<fuchsia_media2::StreamSink>();
  auto builder = fuchsia_audio_mixer::wire::StreamSinkProducer::Builder(arena);
  builder.server_end(std::move(stream_sink_server));
  builder.format(kFormat.ToWireFidl(arena));
  builder.reference_clock(MakeReferenceClock(arena));
  builder.payload_buffer(MakeVmo());
  builder.media_ticks_per_second_numerator(1);
  builder.media_ticks_per_second_denominator(1);
  return builder;
}

fidl::WireTableBuilder<fuchsia_audio_mixer::wire::StreamSinkConsumer> MakeDefaultStreamSinkConsumer(
    fidl::AnyArena& arena) {
  auto [stream_sink_client, stream_sink_server] = CreateClientOrDie<fuchsia_media2::StreamSink>();
  auto builder = fuchsia_audio_mixer::wire::StreamSinkConsumer::Builder(arena);
  builder.client_end(stream_sink_client.TakeClientEnd());
  builder.format(kFormat.ToWireFidl(arena));
  builder.reference_clock(MakeReferenceClock(arena));
  builder.payload_buffer(MakeVmo());
  builder.media_ticks_per_second_numerator(1);
  builder.media_ticks_per_second_denominator(1);
  return builder;
}

fidl::WireTableBuilder<fuchsia_audio::wire::RingBuffer> MakeDefaultRingBuffer(
    fidl::AnyArena& arena) {
  auto builder = fuchsia_audio::wire::RingBuffer::Builder(arena);
  builder.vmo(MakeVmo(1024));
  builder.format(kFormat.ToWireFidl(arena));
  builder.producer_bytes(512);
  builder.consumer_bytes(512);
  builder.reference_clock(MakeClock());
  return builder;
}

fidl::WireTableBuilder<fuchsia_audio_mixer::wire::GraphCreateThreadRequest>
MakeDefaultCreateThreadRequest(fidl::AnyArena& arena) {
  auto builder = fuchsia_audio_mixer::wire::GraphCreateThreadRequest::Builder(arena);
  builder.name(fidl::StringView::FromExternal("thread"));
  builder.period(zx::msec(10).to_nsecs());
  builder.cpu_per_period(zx::msec(5).to_nsecs());
  return builder;
}

fidl::WireTableBuilder<fuchsia_audio_effects::wire::ProcessorConfiguration>
MakeDefaultProcessorConfig(fidl::AnyArena& arena) {
  auto builder = fuchsia_audio_effects::wire::ProcessorConfiguration::Builder(arena);
  builder.block_size_frames(1);
  builder.max_frames_per_call(1);

  fidl::VectorView<fuchsia_audio_effects::wire::InputConfiguration> inputs(arena, 1);
  inputs.at(0) = fuchsia_audio_effects::wire::InputConfiguration::Builder(arena)
                     .buffer(fuchsia_mem::wire::Range{.vmo = MakeVmo(), .offset = 0, .size = 1024})
                     .format(kFormat.ToLegacyFidl())
                     .Build();
  builder.inputs(fidl::ObjectView{arena, inputs});

  fidl::VectorView<fuchsia_audio_effects::wire::OutputConfiguration> outputs(arena, 1);
  outputs.at(0) = fuchsia_audio_effects::wire::OutputConfiguration::Builder(arena)
                      .buffer(fuchsia_mem::wire::Range{.vmo = MakeVmo(), .offset = 0, .size = 1024})
                      .format(kFormat.ToLegacyFidl())
                      .latency_frames(0)
                      .ring_out_frames(0)
                      .Build();
  builder.outputs(fidl::ObjectView{arena, outputs});

  auto endpoints = fidl::CreateEndpoints<fuchsia_audio_effects::Processor>();
  FX_CHECK(endpoints.is_ok());
  builder.processor(std::move(endpoints->client));

  return builder;
}

// Testing strategy: we test all error cases implemented in graph_server.cc and very high-level
// success cases. We leave graph behavior testing (e.g. mixing) for integration tests.
class GraphServerTest : public ::testing::Test {
 public:
  GraphServer& server() { return wrapper_.server(); }
  fidl::WireSyncClient<fuchsia_audio_mixer::Graph>& client() { return wrapper_.client(); }

  void CreateProducerAndConsumer(NodeId* producer_id, NodeId* consumer_id);

 protected:
  fidl::Arena<> arena_;

 private:
  std::shared_ptr<FidlThread> thread_ = FidlThread::CreateFromNewThread("test_fidl_thread");
  std::shared_ptr<FidlThread> realtime_thread_ =
      FidlThread::CreateFromNewThread("test_realtime_fidl_thread");
  TestServerAndClient<GraphServer> wrapper_{
      thread_, GraphServer::Args{
                   .realtime_fidl_thread = realtime_thread_,
                   .clock_factory = std::make_shared<RealClockFactory>(),
                   .clock_registry = std::make_shared<ClockRegistry>(),
               }};
};

//
// CreateProducer
//

TEST_F(GraphServerTest, CreateProducerFailsMissingDirection) {
  auto [stream_sink_client, stream_sink_server] = CreateClientOrDie<fuchsia_media2::StreamSink>();

  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          // no direction()
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithStreamSink(
              arena_, MakeDefaultStreamSinkProducer(arena_).Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateProducerFailsMissingDataSource) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          // no data_source()
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateProducerFailsUnknownDataSource) {
  using ProducerDataSource = ::fuchsia_audio_mixer::wire::ProducerDataSource;

  // TODO(fxbug.dev/109467): When this bug is fixed, this code can be simplified. For now we need to
  // create an "unknown" variant manually.
  struct RawProducerDataSource {
    fidl_xunion_tag_t ordinal;
    FIDL_ALIGNDECL fidl::UntypedEnvelope envelope;
  };
  // C++ type punning requires using std::memcpy.
  RawProducerDataSource raw{
      .ordinal = static_cast<fidl_xunion_tag_t>(ProducerDataSource::Tag::kUnknown),
  };
  ProducerDataSource data_source;
  static_assert(sizeof(data_source) == sizeof(raw));
  std::memcpy(&data_source, &raw, sizeof(data_source));

  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fidl::ObjectView<ProducerDataSource>::FromExternal(&data_source))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kUnsupportedOption);
}

TEST_F(GraphServerTest, CreateProducerStreamSinkFailsBadFields) {
  struct TestCase {
    std::string name;
    std::function<void(fidl::WireTableBuilder<fuchsia_audio_mixer::wire::StreamSinkProducer>&)>
        edit;
    CreateNodeError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "MissingFormat",
          .edit =
              [](auto data_source) {
                data_source.format(fidl::ObjectView<fuchsia_audio::wire::Format>());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingReferenceClock",
          .edit =
              [](auto data_source) {
                data_source.reference_clock(
                    fidl::ObjectView<fuchsia_audio_mixer::wire::ReferenceClock>());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingReferenceClockHandle",
          .edit =
              [this](auto data_source) {
                data_source.reference_clock(
                    fuchsia_audio_mixer::wire::ReferenceClock::Builder(arena_).Build());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingTicksPerSecondNumerator",
          .edit =
              [](auto data_source) {
                data_source.media_ticks_per_second_numerator(fidl::ObjectView<uint64_t>());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingTicksPerSecondDenominator",
          .edit =
              [](auto data_source) {
                data_source.media_ticks_per_second_denominator(fidl::ObjectView<uint64_t>());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "InvalidFormat",
          .edit = [this](auto data_source) { data_source.format(MakeInvalidFormatFidl(arena_)); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "InvalidPayloadBuffer",
          .edit = [](auto data_source) { data_source.payload_buffer(MakeInvalidVmo()); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "InvalidMediaTicksPerSecondNumerator",
          .edit = [](auto data_source) { data_source.media_ticks_per_second_numerator(0); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "InvalidMediaTicksPerSecondDenominator",
          .edit = [](auto data_source) { data_source.media_ticks_per_second_denominator(0); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);
    auto data_source = MakeDefaultStreamSinkProducer(arena_);
    tc.edit(data_source);

    auto result = client()->CreateProducer(
        fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("producer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithStreamSink(
                arena_, data_source.Build()))
            .Build());

    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "CreateProducer did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

// TODO(fxbug.dev/109458): can be merged into CreateProducerStreamSinkFailsBadFields after fix.
TEST_F(GraphServerTest, CreateProducerStreamSinkFailsMissingServerEnd) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithStreamSink(
              arena_, fuchsia_audio_mixer::wire::StreamSinkProducer::Builder(arena_)
                          // no server_end()
                          .format(kFormat.ToWireFidl(arena_))
                          .reference_clock(MakeReferenceClock(arena_))
                          .payload_buffer(MakeVmo())
                          .media_ticks_per_second_numerator(1)
                          .media_ticks_per_second_denominator(1)
                          .Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

// TODO(fxbug.dev/109458): can be merged into CreateProducerStreamSinkFailsBadFields after fix.
TEST_F(GraphServerTest, CreateProducerStreamSinkFailsMissingPayloadBuffer) {
  auto [stream_sink_client, stream_sink_server] = CreateClientOrDie<fuchsia_media2::StreamSink>();
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithStreamSink(
              arena_, fuchsia_audio_mixer::wire::StreamSinkProducer::Builder(arena_)
                          .server_end(std::move(stream_sink_server))
                          .format(kFormat.ToWireFidl(arena_))
                          .reference_clock(MakeReferenceClock(arena_))
                          // no payload_buffer()
                          .media_ticks_per_second_numerator(1)
                          .media_ticks_per_second_denominator(1)
                          .Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateProducerStreamSinkSuccess) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithStreamSink(
              arena_, MakeDefaultStreamSinkProducer(arena_).Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_FALSE(result->is_error()) << result->error_value();
  ASSERT_TRUE(result->value()->has_id());
  EXPECT_EQ(result->value()->id(), 1u);
}

TEST_F(GraphServerTest, CreateProducerRingBufferFailsBadFields) {
  struct TestCase {
    std::string name;
    std::function<void(fidl::WireTableBuilder<fuchsia_audio::wire::RingBuffer>&)> edit;
    CreateNodeError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "MissingFormat",
          .edit =
              [](auto ring_buffer) {
                ring_buffer.format(fidl::ObjectView<fuchsia_audio::wire::Format>());
              },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingProducerBytes",
          .edit =
              [](auto ring_buffer) { ring_buffer.producer_bytes(fidl::ObjectView<uint64_t>()); },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "MissingConsumerBytes",
          .edit =
              [](auto ring_buffer) { ring_buffer.consumer_bytes(fidl::ObjectView<uint64_t>()); },
          .expected_error = CreateNodeError::kMissingRequiredField,
      },
      {
          .name = "InvalidFormat",
          .edit = [this](auto ring_buffer) { ring_buffer.format(MakeInvalidFormatFidl(arena_)); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "InvalidVmo",
          .edit = [](auto ring_buffer) { ring_buffer.vmo(MakeInvalidVmo()); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "ProducerBytesSpansNonIntergalFrames",
          .edit =
              [](auto ring_buffer) { ring_buffer.producer_bytes(kFormat.bytes_per_frame() + 1); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "ConsumerBytesSpansNonIntergalFrames",
          .edit =
              [](auto ring_buffer) { ring_buffer.consumer_bytes(kFormat.bytes_per_frame() + 1); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "ProducerPlusConsumerBytesTooBig",
          .edit =
              [](auto ring_buffer) {
                ring_buffer.vmo(MakeVmo(1024));
                ring_buffer.producer_bytes(512);
                ring_buffer.consumer_bytes(513);
              },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
      {
          .name = "VmoTooSmall",
          .edit = [](auto ring_buffer) { ring_buffer.vmo(MakeVmo(kFormat.bytes_per_frame() - 1)); },
          .expected_error = CreateNodeError::kInvalidParameter,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);
    auto ring_buffer = MakeDefaultRingBuffer(arena_);
    tc.edit(ring_buffer);

    auto result = client()->CreateProducer(
        fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("producer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
                arena_, ring_buffer.Build()))
            .Build());

    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "CreateProducer did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

// TODO(fxbug.dev/109458): can be merged into CreateProducerRingBufferFailsBadFields after fix.
TEST_F(GraphServerTest, CreateProducerRingBufferFailsMissingVmo) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
              arena_, fuchsia_audio::wire::RingBuffer::Builder(arena_)
                          // no vmo()
                          .format(kFormat.ToWireFidl(arena_))
                          .producer_bytes(512)
                          .consumer_bytes(512)
                          .reference_clock(MakeClock())
                          .Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

// TODO(fxbug.dev/109458): can be merged into CreateProducerRingBufferFailsBadFields after fix.
TEST_F(GraphServerTest, CreateProducerRingBufferFailsMissingReferenceClock) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
              arena_, fuchsia_audio::wire::RingBuffer::Builder(arena_)
                          .vmo(MakeVmo(1024))
                          .format(kFormat.ToWireFidl(arena_))
                          .producer_bytes(512)
                          .consumer_bytes(512)
                          // no reference_clock()
                          .Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateProducerRingBufferSuccess) {
  auto result = client()->CreateProducer(
      fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("producer"))
          .direction(PipelineDirection::kOutput)
          .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
              arena_, MakeDefaultRingBuffer(arena_).Build()))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_FALSE(result->is_error()) << result->error_value();
  ASSERT_TRUE(result->value()->has_id());
  EXPECT_EQ(result->value()->id(), 1u);
}

//
// CreateConsumer
//
// Since CreateProducer and CreateConsumer share most of the same validation code, CreateConsumer's
// "BadFields" tests are mostly covered by tests above. We don't bother repeating those cases here.
//

TEST_F(GraphServerTest, CreateConsumerFailsBadFields) {
  // Each consumer needs a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  // TODO(fxbug.dev/109458): can be switch to a table-driven test after fix.
  {
    SCOPED_TRACE("MissingDirection");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            // no direction()
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
  }

  {
    SCOPED_TRACE("MissingDataSource");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            // no data_source()
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
  }

  {
    SCOPED_TRACE("MissingStreamSinkClientEnd");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithStreamSink(
                arena_, fuchsia_audio_mixer::wire::StreamSinkConsumer::Builder(arena_)
                            // no client_end()
                            .format(kFormat.ToWireFidl(arena_))
                            .reference_clock(MakeReferenceClock(arena_))
                            .payload_buffer(MakeVmo())
                            .media_ticks_per_second_numerator(1)
                            .media_ticks_per_second_denominator(1)
                            .Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
  }

  {
    SCOPED_TRACE("MissingOptions");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            // no options()
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
  }

  {
    SCOPED_TRACE("MissingThread");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_)
                         // no thread()
                         .Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
  }

  {
    SCOPED_TRACE("UnknownThread");

    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_)
                         .thread(2)  // non-existent thread
                         .Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), CreateNodeError::kInvalidParameter);
  }
}

TEST_F(GraphServerTest, CreateConsumerStreamSinkSuccess) {
  // Each consumer needs a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  {
    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithStreamSink(
                arena_, MakeDefaultStreamSinkConsumer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }
}

TEST_F(GraphServerTest, CreateConsumerRingBufferSuccess) {
  // Each consumer needs a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  {
    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }
}

//
// DeleteNode
//

TEST_F(GraphServerTest, DeleteNodeFails) {
  using DeleteNodeError = fuchsia_audio_mixer::DeleteNodeError;

  // This only tests error cases detected by GraphServer::DeleteNode. Other error cases are detected
  // by Node::DeleteNode -- those cases are tested in node_unittest.cc.
  struct TestCase {
    std::string name;
    std::optional<NodeId> id;
    DeleteNodeError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "Missing id",
          .expected_error = DeleteNodeError::kDoesNotExist,
      },
      {
          .name = "Invalid id",
          .id = 99,
          .expected_error = DeleteNodeError::kDoesNotExist,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);

    auto builder = fuchsia_audio_mixer::wire::GraphDeleteNodeRequest::Builder(arena_);
    if (tc.id) {
      builder.id(*tc.id);
    }

    auto result = client()->DeleteNode(builder.Build());
    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "DeleteNode did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

TEST_F(GraphServerTest, DeleteNodeSuccess) {
  NodeId id;

  // Create a producer node.
  {
    auto result = client()->CreateProducer(
        fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("producer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    id = result->value()->id();
  }

  // Delete that node.
  {
    auto result = client()->DeleteNode(
        fuchsia_audio_mixer::wire::GraphDeleteNodeRequest::Builder(arena_).id(id).Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }
}

// TODO(fxbug.dev/87651): after CreateEdge is implemented, DeleteNodeSuccess should verify that
// CreateEdge fails when passed the deleted node ID

//
// CreateEdge
//

void GraphServerTest::CreateProducerAndConsumer(NodeId* producer_id, NodeId* consumer_id) {
  // Each consumer needs a thread.
  ThreadId thread_id;
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    thread_id = result->value()->id();
  }

  // Producer.
  {
    auto result = client()->CreateProducer(
        fuchsia_audio_mixer::wire::GraphCreateProducerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("producer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ProducerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    *producer_id = result->value()->id();
  }

  // Consumer.
  {
    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_)
                         .thread(thread_id)
                         .Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    *consumer_id = result->value()->id();
  }
}

TEST_F(GraphServerTest, CreateEdgeFails) {
  using CreateEdgeError = fuchsia_audio_mixer::CreateEdgeError;

  NodeId producer_id;
  NodeId consumer_id;
  ASSERT_NO_FATAL_FAILURE(CreateProducerAndConsumer(&producer_id, &consumer_id));

  // This only tests error cases detected by GraphServer::CreateEdge. Other error cases are detected
  // by Node::CreateEdge -- those cases are tested in node_unittest.cc.
  struct TestCase {
    std::string name;
    std::optional<NodeId> source_id;
    std::optional<NodeId> dest_id;
    CreateEdgeError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "Missing source_id",
          .dest_id = consumer_id,
          .expected_error = CreateEdgeError::kInvalidSourceId,
      },
      {
          .name = "Missing dest_id",
          .source_id = producer_id,
          .expected_error = CreateEdgeError::kInvalidDestId,
      },
      {
          .name = "Invalid dest_id",
          .source_id = 99,
          .dest_id = consumer_id,
          .expected_error = CreateEdgeError::kInvalidSourceId,
      },
      {
          .name = "Invalid dest_id",
          .source_id = producer_id,
          .dest_id = 99,
          .expected_error = CreateEdgeError::kInvalidDestId,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);

    auto builder = fuchsia_audio_mixer::wire::GraphCreateEdgeRequest::Builder(arena_);
    if (tc.source_id) {
      builder.source_id(*tc.source_id);
    }
    if (tc.dest_id) {
      builder.dest_id(*tc.dest_id);
    }

    auto result = client()->CreateEdge(builder.Build());
    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "CreateEdge did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

TEST_F(GraphServerTest, CreateEdgeSuccess) {
  NodeId producer_id;
  NodeId consumer_id;
  ASSERT_NO_FATAL_FAILURE(CreateProducerAndConsumer(&producer_id, &consumer_id));

  auto result =
      client()->CreateEdge(fuchsia_audio_mixer::wire::GraphCreateEdgeRequest::Builder(arena_)
                               .source_id(producer_id)
                               .dest_id(consumer_id)
                               .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_FALSE(result->is_error()) << result->error_value();
}

//
// DeleteEdge
//

TEST_F(GraphServerTest, DeleteEdgeFails) {
  using DeleteEdgeError = fuchsia_audio_mixer::DeleteEdgeError;

  NodeId producer_id;
  NodeId consumer_id;
  ASSERT_NO_FATAL_FAILURE(CreateProducerAndConsumer(&producer_id, &consumer_id));

  // Start with an edge.
  {
    auto result =
        client()->CreateEdge(fuchsia_audio_mixer::wire::GraphCreateEdgeRequest::Builder(arena_)
                                 .source_id(producer_id)
                                 .dest_id(consumer_id)
                                 .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }

  // This only tests error cases detected by GraphServer::DeleteEdge. Other error cases are detected
  // by Node::DeleteEdge -- those cases are tested in node_unittest.cc.
  struct TestCase {
    std::string name;
    std::optional<NodeId> source_id;
    std::optional<NodeId> dest_id;
    DeleteEdgeError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "Missing source_id",
          .dest_id = consumer_id,
          .expected_error = DeleteEdgeError::kInvalidSourceId,
      },
      {
          .name = "Missing dest_id",
          .source_id = producer_id,
          .expected_error = DeleteEdgeError::kInvalidDestId,
      },
      {
          .name = "Invalid dest_id",
          .source_id = 99,
          .dest_id = consumer_id,
          .expected_error = DeleteEdgeError::kInvalidSourceId,
      },
      {
          .name = "Invalid dest_id",
          .source_id = producer_id,
          .dest_id = 99,
          .expected_error = DeleteEdgeError::kInvalidDestId,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);

    auto builder = fuchsia_audio_mixer::wire::GraphDeleteEdgeRequest::Builder(arena_);
    if (tc.source_id) {
      builder.source_id(*tc.source_id);
    }
    if (tc.dest_id) {
      builder.dest_id(*tc.dest_id);
    }

    auto result = client()->DeleteEdge(builder.Build());
    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "DeleteEdge did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

TEST_F(GraphServerTest, DeleteEdgeSuccess) {
  NodeId producer_id;
  NodeId consumer_id;
  ASSERT_NO_FATAL_FAILURE(CreateProducerAndConsumer(&producer_id, &consumer_id));

  // Start the edge.
  {
    auto result =
        client()->CreateEdge(fuchsia_audio_mixer::wire::GraphCreateEdgeRequest::Builder(arena_)
                                 .source_id(producer_id)
                                 .dest_id(consumer_id)
                                 .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }

  // Delete it.
  {
    auto result =
        client()->DeleteEdge(fuchsia_audio_mixer::wire::GraphDeleteEdgeRequest::Builder(arena_)
                                 .source_id(producer_id)
                                 .dest_id(consumer_id)
                                 .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }
}

//
// CreateCustom
//

TEST_F(GraphServerTest, CreateCustomFailsMissingReferenceClock) {
  auto result = client()->CreateCustom(
      fuchsia_audio_mixer::wire::GraphCreateCustomRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("custom"))
          .direction(PipelineDirection::kInput)
          .config(MakeDefaultProcessorConfig(arena_).Build())
          .consumer(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
          // no reference_clock()
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateCustomFailsMissingDirection) {
  auto result = client()->CreateCustom(
      fuchsia_audio_mixer::wire::GraphCreateCustomRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("custom"))
          // no direction()
          .config(MakeDefaultProcessorConfig(arena_).Build())
          .consumer(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
          .reference_clock(MakeReferenceClock(arena_))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateCustomFailsMissingConfig) {
  auto result = client()->CreateCustom(
      fuchsia_audio_mixer::wire::GraphCreateCustomRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("custom"))
          .direction(PipelineDirection::kInput)
          // no config()
          .consumer(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
          .reference_clock(MakeReferenceClock(arena_))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kMissingRequiredField);
}

TEST_F(GraphServerTest, CreateCustomFailsInvalidConfig) {
  auto result = client()->CreateCustom(
      fuchsia_audio_mixer::wire::GraphCreateCustomRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("custom"))
          .direction(PipelineDirection::kInput)
          .config(MakeDefaultProcessorConfig(arena_).block_size_frames(-1).Build())
          .consumer(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
          .reference_clock(MakeReferenceClock(arena_))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), CreateNodeError::kInvalidParameter);
}

TEST_F(GraphServerTest, CreateCustomSuccess) {
  auto result = client()->CreateCustom(
      fuchsia_audio_mixer::wire::GraphCreateCustomRequest::Builder(arena_)
          .name(fidl::StringView::FromExternal("custom"))
          .direction(PipelineDirection::kInput)
          .config(MakeDefaultProcessorConfig(arena_).Build())
          .consumer(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
          .reference_clock(MakeReferenceClock(arena_))
          .Build());

  ASSERT_TRUE(result.ok()) << result;
  ASSERT_FALSE(result->is_error()) << result->error_value();
  EXPECT_TRUE(result->value()->has_id());
  ASSERT_TRUE(result->value()->has_node_properties());
  EXPECT_EQ(result->value()->node_properties().source_ids().count(), 1ul);
  EXPECT_EQ(result->value()->node_properties().dest_ids().count(), 1ul);
}

//
// CreateThread
//

TEST_F(GraphServerTest, CreateThreadFailsBadFields) {
  using CreateThreadError = fuchsia_audio_mixer::CreateThreadError;

  struct TestCase {
    std::string name;
    std::function<void(
        fidl::WireTableBuilder<fuchsia_audio_mixer::wire::GraphCreateThreadRequest>&)>
        edit;
    CreateThreadError expected_error;
  };
  std::vector<TestCase> cases = {
      {
          .name = "MissingPeriod",
          .edit = [](auto request) { request.period(fidl::ObjectView<int64_t>()); },
          .expected_error = CreateThreadError::kMissingRequiredField,
      },
      {
          .name = "MissingCpuPerPeriod",
          .edit = [](auto request) { request.cpu_per_period(fidl::ObjectView<int64_t>()); },
          .expected_error = CreateThreadError::kMissingRequiredField,
      },
      {
          .name = "ZeroPeriod",
          .edit = [](auto request) { request.period(0); },
          .expected_error = CreateThreadError::kInvalidParameter,
      },
      {
          .name = "ZeroCpuPerPeriod",
          .edit = [](auto request) { request.cpu_per_period(0); },
          .expected_error = CreateThreadError::kInvalidParameter,
      },
      {
          .name = "NegativePeriod",
          .edit = [](auto request) { request.period(-1); },
          .expected_error = CreateThreadError::kInvalidParameter,
      },
      {
          .name = "NegativeCpuPerPeriod",
          .edit = [](auto request) { request.cpu_per_period(-1); },
          .expected_error = CreateThreadError::kInvalidParameter,
      },
      {
          .name = "CpuPerPeriodTooBig",
          .edit =
              [](auto request) {
                request.period(10);
                request.cpu_per_period(11);
              },
          .expected_error = CreateThreadError::kInvalidParameter,
      },
  };

  for (auto& tc : cases) {
    SCOPED_TRACE("TestCase: " + tc.name);
    auto request = MakeDefaultCreateThreadRequest(arena_);
    tc.edit(request);

    auto result = client()->CreateThread(request.Build());
    if (!result.ok()) {
      ADD_FAILURE() << "failed to send method call: " << result;
      continue;
    }
    if (!result->is_error()) {
      ADD_FAILURE() << "CreateThread did not fail";
      continue;
    }
    EXPECT_EQ(result->error_value(), tc.expected_error);
  }
}

TEST_F(GraphServerTest, CreateThreadSuccess) {
  auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
  ASSERT_TRUE(result.ok()) << result;
  ASSERT_FALSE(result->is_error()) << result->error_value();
  ASSERT_TRUE(result->value()->has_id());
  EXPECT_EQ(result->value()->id(), 1u);
}

//
// DeleteThread
//

TEST_F(GraphServerTest, DeleteThreadFailsMissingId) {
  auto result = client()->DeleteThread(
      fuchsia_audio_mixer::wire::GraphDeleteThreadRequest::Builder(arena_).Build());
  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), fuchsia_audio_mixer::DeleteThreadError::kInvalidId);
}

TEST_F(GraphServerTest, DeleteThreadFailsIdNotFound) {
  auto result = client()->DeleteThread(
      fuchsia_audio_mixer::wire::GraphDeleteThreadRequest::Builder(arena_).id(1).Build());
  ASSERT_TRUE(result.ok()) << result;
  ASSERT_TRUE(result->is_error());
  ASSERT_EQ(result->error_value(), fuchsia_audio_mixer::DeleteThreadError::kInvalidId);
}

TEST_F(GraphServerTest, DeleteThreadFailsStillInUse) {
  // Create a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  // Attach a consumer.
  {
    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }

  // Delete should fail.
  {
    auto result = client()->DeleteThread(
        fuchsia_audio_mixer::wire::GraphDeleteThreadRequest::Builder(arena_).id(1).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_TRUE(result->is_error());
    ASSERT_EQ(result->error_value(), fuchsia_audio_mixer::DeleteThreadError::kStillInUse);
  }
}

TEST_F(GraphServerTest, DeleteThreadSuccess) {
  // Create a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  // Delete it.
  {
    auto result = client()->DeleteThread(
        fuchsia_audio_mixer::wire::GraphDeleteThreadRequest::Builder(arena_).id(1).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }
}

TEST_F(GraphServerTest, DeleteThreadSuccessAfterConsumerDeleted) {
  // Create a thread.
  {
    auto result = client()->CreateThread(MakeDefaultCreateThreadRequest(arena_).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    ASSERT_TRUE(result->value()->has_id());
    EXPECT_EQ(result->value()->id(), 1u);
  }

  // Attach a consumer.
  {
    auto result = client()->CreateConsumer(
        fuchsia_audio_mixer::wire::GraphCreateConsumerRequest::Builder(arena_)
            .name(fidl::StringView::FromExternal("consumer"))
            .direction(PipelineDirection::kOutput)
            .data_source(fuchsia_audio_mixer::wire::ConsumerDataSource::WithRingBuffer(
                arena_, MakeDefaultRingBuffer(arena_).Build()))
            .options(fuchsia_audio_mixer::wire::ConsumerOptions::Builder(arena_).thread(1).Build())
            .Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
    EXPECT_EQ(result->value()->id(), 1u);
  }

  // Delete that consumer.
  {
    auto result = client()->DeleteNode(
        fuchsia_audio_mixer::wire::GraphDeleteNodeRequest::Builder(arena_).id(1).Build());

    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }

  // Deleting the thread should succeed.
  {
    auto result = client()->DeleteThread(
        fuchsia_audio_mixer::wire::GraphDeleteThreadRequest::Builder(arena_).id(1).Build());
    ASSERT_TRUE(result.ok()) << result;
    ASSERT_FALSE(result->is_error()) << result->error_value();
  }
}

}  // namespace
}  // namespace media_audio
