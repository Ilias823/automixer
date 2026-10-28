#include "Updater.h"

namespace am
{

ReleaseInfo fetchLatestRelease (const juce::String& repo)
{
    ReleaseInfo info;
    int status = 0;
    const juce::URL url ("https://api.github.com/repos/" + repo + "/releases/latest");
    auto options = juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
                       .withExtraHeaders ("Accept: application/vnd.github+json\r\nUser-Agent: Automixer-plugin")
                       .withConnectionTimeoutMs (8000)
                       .withStatusCode (&status);

    std::unique_ptr<juce::InputStream> stream (url.createInputStream (options));
    if (stream == nullptr)
    {
        info.error = "No connection to GitHub.";
        return info;
    }
    const auto body = stream->readEntireStreamAsString();
    if (status == 404)
    {
        info.error = "No release published yet.";
        return info;
    }
    if (status != 200)
    {
        info.error = "GitHub answered " + juce::String (status) + ".";
        return info;
    }

    const auto json = juce::JSON::parse (body);
    info.version = json["tag_name"].toString().trimCharactersAtStart ("vV");
    info.pageUrl = json["html_url"].toString();
    if (auto* assets = json["assets"].getArray())
        for (const auto& a : *assets)
        {
            const auto link = a["browser_download_url"].toString();
            if (link.endsWithIgnoreCase (".pkg"))
                info.pkgUrl = link;
        }
    info.ok = info.version.isNotEmpty();
    if (! info.ok)
        info.error = "Could not read the release.";
    return info;
}

int compareVersions (const juce::String& a, const juce::String& b)
{
    juce::StringArray pa, pb;
    pa.addTokens (a, ".", "");
    pb.addTokens (b, ".", "");
    for (int i = 0; i < juce::jmax (pa.size(), pb.size()); ++i)
    {
        const int x = i < pa.size() ? pa[i].getIntValue() : 0;
        const int y = i < pb.size() ? pb[i].getIntValue() : 0;
        if (x != y)
            return x < y ? -1 : 1;
    }
    return 0;
}

} // namespace am
