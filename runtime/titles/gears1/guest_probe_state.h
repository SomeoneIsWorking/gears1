#pragma once

#include <cstdint>

namespace gears::titles::gears1
{

void NotePoolEvent(std::uint32_t address, bool freed);
bool LastPoolEventWasFree(std::uint32_t address, std::uint64_t &ordinal, bool &known);
void ReportLastFree(std::uint32_t address);
void ReportArchiveLifetime(std::uint32_t object);
void NoteArchiveAsync(std::uint32_t self, std::uint32_t name);

void CountRingProducerEntry();
void CountRingProducerOverlap();
std::uint64_t RingProducerEntries();
std::uint64_t RingProducerOverlaps();
void NoteRenderRingReservation(std::uint32_t start, std::uint32_t bytes, std::uint32_t caller,
                               std::uint32_t recordOwner, std::uint32_t recordVtable,
                               std::uint32_t recordBase, std::uint32_t recordIndex,
                               std::uint32_t companionWord);
void ReportRenderRingReservationForObject(std::uint32_t object);

void ReportLinkerState(std::uint32_t holder);
void ReportMapChangeSeams();
void ReportMapNameProbe();
void ReportFStringProbe();
void ReportLoaderThunks();
void ReportEarlyThunks();

} // namespace gears::titles::gears1
