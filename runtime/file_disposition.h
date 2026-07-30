// NtCreateFile's CreateDisposition, which decides whether a missing file is an
// error or a file to make.
//
// The runtime ignored this argument entirely: NtCreateFile never read r10, and
// its rule was "if the file is missing and the mount is writable, make it". That
// is right for a save being written and wrong for every read. A caller asking to
// open an existing file for reading got a newly created empty one and
// STATUS_SUCCESS -- an honest failure converted into a success carrying no data,
// which is the single most expensive shape a host shim can have. The title reads
// zero bytes, believes the load worked, and continues on empty state.
//
// Split into its own header so the decision is testable without a filesystem: it
// is a pure function of the disposition, whether the file is there, and whether
// the mount can be written.
#pragma once

#include <cstdint>

namespace gears
{

// NT's dispositions. NOT the console's XCONTENT dispositions in xam_content.h --
// those use different numbers for different meanings, and confusing the two is
// both easy and expensive.
enum FileDisposition : uint32_t
{
    kFileSupersede = 0,     // replace it if present, else create
    kFileOpen = 1,          // open it; FAIL if absent
    kFileCreate = 2,        // create it; FAIL if present
    kFileOpenIf = 3,        // open it, creating if needed
    kFileOverwrite = 4,     // truncate it; FAIL if absent
    kFileOverwriteIf = 5,   // truncate it, creating if needed
};

enum class FileOpenAction
{
    OpenExisting,   // open in place, keeping the contents
    CreateNew,      // make a new, empty file
    Truncate,       // open and discard the contents
    NotFound,       // refuse: STATUS_OBJECT_NAME_NOT_FOUND
    AlreadyExists,  // refuse: STATUS_OBJECT_NAME_COLLISION
};

// Decides what an open should do. `writable` is whether the MOUNT permits
// writes, so the disc refuses creation and truncation while still serving reads.
//
// An unrecognised disposition is REFUSED rather than guessed at. The console
// defines six; a seventh means the caller is not doing what we think it is, and
// picking a convenient behaviour there is how a wrong assumption stops being
// visible.
FileOpenAction DecideFileOpen(FileDisposition disposition, bool exists,
                              bool writable);

} // namespace gears
