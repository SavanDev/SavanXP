#pragma once

namespace i386 {
struct ContextFrame;
namespace clock32 {
ContextFrame* tick(ContextFrame* frame);
}
}
