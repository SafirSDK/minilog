/******************************************************************************
 *
 * Copyright Saab AB, 2026 (https://github.com/SafirSDK/minilog)
 *
 * Created by: Lars Hagström / lars@foldspace.nu
 *
 *******************************************************************************
 *
 * This file is part of minilog.
 *
 * minilog is released under the MIT License. See the LICENSE file in
 * the project root for full license information.
 *
 ******************************************************************************/

#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>

namespace minilog
{

// Admission control for the UDP receive path.
//
// Nothing between the socket and the disk applies back pressure: the receive
// handler copies each datagram and posts it to the io_context, the worker that
// picks it up posts a copy to every matching sink strand, and the sink flushes
// after each line so tail sees entries immediately. The bottom of that pipe is
// far narrower than the top, so a sender faster than the disk grows the process
// until it is killed. UDP gives the sender no way to learn either way and the
// kernel is already dropping silently when its own socket buffer fills, so the
// answer is to drop deliberately, at a limit we picked, and stay alive.
//
// admit() charges a datagram's size against a byte budget and hands back a
// token. Every copy of the message carries the token, so the charge is only
// released once the last queued copy is gone — which is the point: bounding the
// io_context queue alone would just move the growth to the sink strands.
//
// The budget is in datagram bytes, not queued bytes: a queued SyslogMessage
// holds `raw` and `message`, which are near-copies of each other, and one such
// message per sink the datagram routes to. So the memory actually held is a
// small multiple of the budget, and the budget is the knob rather than the
// measurement.
//
// Bytes, not a message count, because a count treats a 30-byte "disk full" and
// a 64 KB stack trace alike: 10k queued small messages is about 1 MB, 10k large
// ones about 1.2 GB.
class AdmissionControl
{
public:
    // A charge against the budget, released when the last copy is destroyed.
    // Opaque so that SyslogMessage can carry one without depending on this
    // header. It owns the counters it decrements, so a token still in a queue
    // when the server goes away stays safe to destroy.
    using Token = std::shared_ptr<const void>;

    explicit AdmissionControl(std::uint64_t budgetBytes)
        : m_state(std::make_shared<State>(budgetBytes))
    {
    }

    AdmissionControl(const AdmissionControl&)            = delete;
    AdmissionControl& operator=(const AdmissionControl&) = delete;

    // Charges bytes against the budget. Returns a null token, and counts a
    // drop, if that would exceed it.
    [[nodiscard]] Token admit(std::size_t bytes)
    {
        auto inFlight = m_state->inFlight.load(std::memory_order_relaxed);
        do
        {
            if (inFlight + bytes > m_state->budget)
            {
                m_state->droppedWindow.fetch_add(1, std::memory_order_relaxed);
                m_state->droppedTotal.fetch_add(1, std::memory_order_relaxed);
                return {};
            }
        } while (!m_state->inFlight.compare_exchange_weak(
            inFlight, inFlight + bytes, std::memory_order_relaxed, std::memory_order_relaxed));

        return std::make_shared<Charge>(m_state, bytes);
    }

    // Number of drops since the last call, which this resets. Reporting every
    // drop would make the notice its own flood, so the caller batches them.
    [[nodiscard]] std::uint64_t takeDropped()
    {
        return m_state->droppedWindow.exchange(0, std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t inFlight() const
    {
        return m_state->inFlight.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t droppedTotal() const
    {
        return m_state->droppedTotal.load(std::memory_order_relaxed);
    }

    [[nodiscard]] std::uint64_t budget() const { return m_state->budget; }

private:
    struct State
    {
        explicit State(std::uint64_t budgetBytes) : budget(budgetBytes) {}

        const std::uint64_t budget;
        std::atomic<std::uint64_t> inFlight{0};
        std::atomic<std::uint64_t> droppedWindow{0};
        std::atomic<std::uint64_t> droppedTotal{0};
    };

    // Holds the charge for as long as any copy of the token exists.
    struct Charge
    {
        Charge(std::shared_ptr<State> stateIn, std::size_t bytesIn)
            : state(std::move(stateIn)), bytes(bytesIn)
        {
        }

        ~Charge() { state->inFlight.fetch_sub(bytes, std::memory_order_relaxed); }

        Charge(const Charge&)            = delete;
        Charge& operator=(const Charge&) = delete;

        std::shared_ptr<State> state;
        std::size_t bytes;
    };

    std::shared_ptr<State> m_state;
};

} // namespace minilog
