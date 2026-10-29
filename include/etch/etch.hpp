// etch - a small C++23 logger. Include this one header to get everything.
//
//   #include <etch/etch.hpp>
//   etch::info("hello {}", "world");                       // default logger, console
//
//   etch::Logger log{"game", {std::make_shared<etch::ConsoleSink>()}};
//   log.warn("fps dropped to {:.1f}", fps);                 // file:line captured, no macro
//   log.hexdump(etch::Level::kDebug, "packet", buffer);     // hex editor style dump
//   auto timer = log.scope("load level");                   // "load level: done in 12.3 ms"
#pragma once

#include "agent.hpp"
#include "async_control.hpp"
#include "async_registry.hpp"
#include "async_sink.hpp"
#include "context.hpp"
#include "debugger_sink.hpp"
#include "default_logger.hpp"
#include "delivery.hpp"
#include "file_sink.hpp"
#include "json.hpp"
#include "level.hpp"
#include "logger.hpp"
#include "rate_limit.hpp"
#include "record.hpp"
#include "record_ring.hpp"
#include "sink_set.hpp"
#include "sinks.hpp"
#include "version.hpp"
