#pragma once

#include <array>
#include <cstdint>

namespace gears::titles::gears1
{

struct RenderRingReservationProvenance
{
    std::uint32_t start = 0;
    std::uint32_t bytes = 0;
    std::uint32_t caller = 0;
    std::uint32_t recordOwner = 0;
    std::uint32_t recordDispatchTable = 0;
    std::uint32_t recordPrimaryDescriptor = 0;
    std::uint32_t recordPrimaryTarget = 0;
    std::uint32_t recordSecondaryTarget = 0;
    std::uint32_t recordPrimaryValue = 0;
    std::uint32_t recordSecondaryValue = 0;
    std::uint32_t recordBase = 0;
    std::uint32_t recordIndex = 0;
    std::uint32_t recordSource = 0;
    std::array<std::uint32_t, 16> recordWords{};
    std::uint32_t companionWord = 0;
};

void NotePoolEvent(std::uint32_t address, bool freed);
bool LastPoolEventWasFree(std::uint32_t address, std::uint64_t &ordinal, bool &known);
void ReportLastFree(std::uint32_t address);
void ReportArchiveLifetime(std::uint32_t object);
void NoteArchiveAsync(std::uint32_t self, std::uint32_t name);

void CountRingProducerEntry();
void CountRingProducerOverlap();
std::uint64_t RingProducerEntries();
std::uint64_t RingProducerOverlaps();
void NoteRenderRingReservation(RenderRingReservationProvenance reservation);
void ReportRenderRingReservationForObject(std::uint32_t object);

void ReportLinkerState(std::uint32_t holder);
void ReportMapChangeSeams();
void ReportMapNameProbe();
void ReportFStringProbe();
void ReportLoaderThunks();
void ReportEarlyThunks();

} // namespace gears::titles::gears1
