// Tests for NtCreateFile's CreateDisposition, which the runtime ignored entirely.
//
// WHAT WAS WRONG. __imp__NtCreateFile read the handle, the object attributes and
// the status block, and never looked at r10 at all. Its whole rule was "if the
// file is missing and the mount is writable, make it" -- which is right for a
// save being written and wrong for everything else. A caller asking to OPEN AN
// EXISTING file for reading was answered with a newly created empty one and
// STATUS_SUCCESS.
//
// That is the worst shape a host shim can have: it converts an honest failure
// into a success carrying no data. The title then reads zero bytes, believes its
// load succeeded, and proceeds on empty state -- and the eventual crash is
// nowhere near the lie. The whole of issue #45 is a chain of exactly that kind,
// so the rule here is not a detail.
//
// The dispositions are NT's, not the console's XCONTENT ones (which live in
// xam_content.h and have different numbers for different meanings -- an easy and
// expensive confusion):
//
//   FILE_SUPERSEDE    0   replace it if it is there, else create
//   FILE_OPEN         1   open it; FAIL if it is not there
//   FILE_CREATE       2   create it; FAIL if it is already there
//   FILE_OPEN_IF      3   open it, creating it if needed
//   FILE_OVERWRITE    4   truncate it; FAIL if it is not there
//   FILE_OVERWRITE_IF 5   truncate it, creating it if needed

#include <cstdio>

#include "file_disposition.h"

namespace
{

int g_failures = 0;

void Check(bool ok, const char* what)
{
    if (!ok)
    {
        printf("FAIL %s\n", what);
        ++g_failures;
    }
}

using gears::FileOpenAction;
using gears::DecideFileOpen;

// The case that caused the damage: a read of a file that is not there, on a
// mount that happens to be writable. Creating it is a lie.
void TestOpenOfAMissingFileFails()
{
    Check(DecideFileOpen(gears::kFileOpen, /*exists=*/false, /*writable=*/true) ==
              FileOpenAction::NotFound,
        "FILE_OPEN of a missing file on a WRITABLE mount must fail -- creating it"
        " reports success with no data, which is how a load silently proceeds on"
        " empty state");
    Check(DecideFileOpen(gears::kFileOpen, false, false) ==
              FileOpenAction::NotFound,
        "FILE_OPEN of a missing file on a read-only mount fails too");
    Check(DecideFileOpen(gears::kFileOpen, true, false) ==
              FileOpenAction::OpenExisting,
        "FILE_OPEN of a file that IS there opens it, read-only mount and all");
}

// FILE_CREATE is the mirror image: it must refuse when the file already exists.
void TestCreateNewRefusesAnExistingFile()
{
    Check(DecideFileOpen(gears::kFileCreate, true, true) ==
              FileOpenAction::AlreadyExists,
        "FILE_CREATE must refuse a file that already exists");
    Check(DecideFileOpen(gears::kFileCreate, false, true) ==
              FileOpenAction::CreateNew,
        "FILE_CREATE creates when the file is absent");
}

// The two "or create" forms, which are what a save being written actually uses,
// and which must keep working -- the title's checkpoint write depends on them.
void TestOpenIfAndOverwriteIfCreate()
{
    Check(DecideFileOpen(gears::kFileOpenIf, false, true) ==
              FileOpenAction::CreateNew,
        "FILE_OPEN_IF creates a missing file");
    Check(DecideFileOpen(gears::kFileOpenIf, true, true) ==
              FileOpenAction::OpenExisting,
        "FILE_OPEN_IF opens an existing one without truncating it");
    Check(DecideFileOpen(gears::kFileOverwriteIf, false, true) ==
              FileOpenAction::CreateNew,
        "FILE_OVERWRITE_IF creates a missing file");
    Check(DecideFileOpen(gears::kFileOverwriteIf, true, true) ==
              FileOpenAction::Truncate,
        "FILE_OVERWRITE_IF truncates an existing one");
    Check(DecideFileOpen(gears::kFileSupersede, true, true) ==
              FileOpenAction::Truncate,
        "FILE_SUPERSEDE replaces an existing file");
    Check(DecideFileOpen(gears::kFileSupersede, false, true) ==
              FileOpenAction::CreateNew,
        "FILE_SUPERSEDE creates an absent one");
}

// FILE_OVERWRITE requires the file to exist, unlike FILE_OVERWRITE_IF. The two
// differ by exactly this and are one digit apart.
void TestOverwriteRequiresTheFile()
{
    Check(DecideFileOpen(gears::kFileOverwrite, false, true) ==
              FileOpenAction::NotFound,
        "FILE_OVERWRITE of a missing file fails -- it is not OVERWRITE_IF");
    Check(DecideFileOpen(gears::kFileOverwrite, true, true) ==
              FileOpenAction::Truncate,
        "FILE_OVERWRITE truncates a file that is there");
}

// Anything that would WRITE must be refused on a read-only mount, whatever the
// disposition asks for. The disc is read-only and a title that tries to create
// there has to be told no rather than quietly succeeding against nothing.
void TestWritesAreRefusedOnAReadOnlyMount()
{
    const gears::FileDisposition writing[] = {
        gears::kFileSupersede, gears::kFileCreate, gears::kFileOpenIf,
        gears::kFileOverwrite, gears::kFileOverwriteIf,
    };
    for (const gears::FileDisposition disposition : writing)
    {
        const FileOpenAction absent = DecideFileOpen(disposition, false, false);
        if (absent != FileOpenAction::NotFound)
        {
            printf("FAIL disposition %u on a read-only mount with no file gave"
                   " %d, expected NotFound\n", unsigned(disposition), int(absent));
            ++g_failures;
        }
    }
    // An EXISTING file on a read-only mount can still be read.
    Check(DecideFileOpen(gears::kFileOpenIf, true, false) ==
              FileOpenAction::OpenExisting,
        "read-only: an existing file is still openable for reading");
    Check(DecideFileOpen(gears::kFileOverwriteIf, true, false) ==
              FileOpenAction::NotFound,
        "read-only: but truncating one is refused");
}

// An unknown disposition must not be treated as "do whatever is convenient".
// The console only defines six, and a seventh means the caller is not doing what
// we think -- guessing there is how a wrong assumption becomes invisible.
void TestUnknownDispositionIsRefused()
{
    Check(DecideFileOpen(gears::FileDisposition(6), false, true) ==
              FileOpenAction::NotFound,
        "an undefined disposition is refused rather than guessed at");
    Check(DecideFileOpen(gears::FileDisposition(99), true, true) ==
              FileOpenAction::NotFound,
        "and so is a wildly out-of-range one, even when the file exists");
}

} // namespace

int main()
{
    TestOpenOfAMissingFileFails();
    TestCreateNewRefusesAnExistingFile();
    TestOpenIfAndOverwriteIfCreate();
    TestOverwriteRequiresTheFile();
    TestWritesAreRefusedOnAReadOnlyMount();
    TestUnknownDispositionIsRefused();

    if (g_failures == 0)
    {
        printf("all file disposition tests passed\n");
        return 0;
    }
    printf("%d file disposition test(s) FAILED\n", g_failures);
    return 1;
}
