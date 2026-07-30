#include "file_disposition.h"

namespace gears
{

FileOpenAction DecideFileOpen(FileDisposition disposition, bool exists,
                              bool writable)
{
    // Whether this disposition would ever touch the file's contents. A read-only
    // mount refuses all of those outright, while still serving an ordinary open
    // of a file that is there.
    const auto refuseIfReadOnly = [writable](FileOpenAction action) {
        return writable ? action : FileOpenAction::NotFound;
    };

    switch (disposition)
    {
    case kFileOpen:
        // The case that caused the damage. A missing file is an ERROR here, on a
        // writable mount as much as anywhere: the caller said open, not create.
        return exists ? FileOpenAction::OpenExisting : FileOpenAction::NotFound;

    case kFileCreate:
        return exists ? FileOpenAction::AlreadyExists
                      : refuseIfReadOnly(FileOpenAction::CreateNew);

    case kFileOpenIf:
        return exists ? FileOpenAction::OpenExisting
                      : refuseIfReadOnly(FileOpenAction::CreateNew);

    case kFileSupersede:
    case kFileOverwriteIf:
        return refuseIfReadOnly(exists ? FileOpenAction::Truncate
                                       : FileOpenAction::CreateNew);

    case kFileOverwrite:
        // Truncating, so it needs the file to be there -- the one digit that
        // separates it from OVERWRITE_IF.
        return exists ? refuseIfReadOnly(FileOpenAction::Truncate)
                      : FileOpenAction::NotFound;
    }

    // Not one of the six. Refused deliberately: see the header.
    return FileOpenAction::NotFound;
}

} // namespace gears
