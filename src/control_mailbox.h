#pragma once

#include "app_config.h"

#include <stdint.h>
#include <type_traits>

namespace app
{

enum class SystemMode : uint8_t
{
    Normal = 0,
    ThermalShutdown = 1
};

// Compact requested-control state crossing from the foreground loop into the
// audio interrupt. Keep this trivially copyable and small so a seqlock retry is
// cheap and no dynamic allocation or mutex is needed.
struct ControlSnapshot
{
    uint8_t effect;
    uint8_t bypass;
    uint8_t system_mode;
    uint8_t reserved;
    uint16_t parameters[config::kPhysicalPotCount];
};

static_assert(std::is_trivially_copyable<ControlSnapshot>::value, "ControlSnapshot must be copied by value");
static_assert(sizeof(ControlSnapshot) <= 16, "ControlSnapshot must stay small for interrupt transfer");
static_assert(sizeof(ControlSnapshot) == 10, "ControlSnapshot layout changed unexpectedly");

// Single-writer/single-reader mailbox for the main loop -> audio callback handoff.
// The main loop is the only writer, and the audio interrupt is the only reader.
class ControlMailbox
{
  public:
    ControlMailbox();

    // Publish() is called from the main loop after controls have been filtered and
    // debounced. It marks the sequence odd while the payload is being copied.
    void Publish(const ControlSnapshot& snapshot);

    // Read() is called once at the beginning of an audio block. It returns false
    // if publication is in progress, allowing the callback to keep its last valid
    // snapshot without blocking.
    bool Read(ControlSnapshot& out, uint32_t& out_revision) const;

  private:
    uint32_t sequence_;
    ControlSnapshot snapshot_;
};

inline ControlMailbox::ControlMailbox()
: sequence_(0),
  snapshot_{config::FX_TUNER, 0, (uint8_t)SystemMode::Normal, 0, {0, 0, 0}}
{
}

// GCC atomics provide the compiler and CPU barriers needed for the callback to
// reject in-progress or torn snapshots. The sequence is even when stable and odd
// while the writer is updating the payload.
inline void ControlMailbox::Publish(const ControlSnapshot& snapshot)
{
    const uint32_t start = __atomic_load_n(&sequence_, __ATOMIC_RELAXED);
    __atomic_store_n(&sequence_, start | 1u, __ATOMIC_RELEASE);
    snapshot_ = snapshot;
    __atomic_store_n(&sequence_, (start + 2u) & ~1u, __ATOMIC_RELEASE);
}

inline bool ControlMailbox::Read(ControlSnapshot& out, uint32_t& out_revision) const
{
    const uint32_t before = __atomic_load_n(&sequence_, __ATOMIC_ACQUIRE);
    if(before & 1u)
        return false;

    const ControlSnapshot copy = snapshot_;
    const uint32_t after = __atomic_load_n(&sequence_, __ATOMIC_ACQUIRE);
    if(before != after || (after & 1u))
        return false;

    out = copy;
    out_revision = after;
    return true;
}

} // namespace app
