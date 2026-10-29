#pragma once

// "Check for Updates": asks GitHub for the newest release of the public
// repository. Runs only when the user presses the button; nothing is
// sent anywhere otherwise.

#include <juce_core/juce_core.h>

namespace am
{

constexpr const char* kReleasesRepo = "Ilias823/automixer";

struct ReleaseInfo
{
    bool ok = false;
    juce::String version;   // "0.2.0" (tag without the leading v)
    juce::String pageUrl;   // release page
    juce::String pkgUrl;    // installer download, if attached
    juce::String error;
};

// Blocking: call it from a background thread.
ReleaseInfo fetchLatestRelease (const juce::String& repo);

// < 0 if a is older than b, 0 if equal, > 0 if newer ("1.2.10" > "1.2.9").
int compareVersions (const juce::String& a, const juce::String& b);

} // namespace am
