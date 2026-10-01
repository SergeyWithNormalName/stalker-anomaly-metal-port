#pragma once

#include "MetalParticleCapture.h"
#include <mutex>
#include <iterator>
#include <string>
#include <utility>
#include <vector>

namespace xray::metal::lensflare
{
struct Draw { std::string texture; particles::SpriteVertex vertices[4]{}; bool afterPostprocess = false; };
struct Frame { std::vector<Draw> draws; };
class Capture {
public:
 void submit(Frame value) { std::lock_guard<std::mutex> lock(m); frame.draws.insert(frame.draws.end(), std::make_move_iterator(value.draws.begin()), std::make_move_iterator(value.draws.end())); }
 Frame drain() { std::lock_guard<std::mutex> lock(m); Frame out; out.draws.swap(frame.draws); return out; }
private: std::mutex m; Frame frame;
};
inline Capture& capture() { static Capture value; return value; }
inline void Submit(Frame value) { capture().submit(std::move(value)); }
inline Frame Drain() { return capture().drain(); }
} // namespace xray::metal::lensflare
