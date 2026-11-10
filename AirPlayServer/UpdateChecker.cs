using System;
using System.Collections.Generic;
using System.Net.Http;
using System.Net.Http.Headers;
using System.Reflection;
using System.Text.Json;
using System.Threading.Tasks;

namespace AirPlayServer;

internal sealed record UpdateInfo(Version Version, string Tag, string PageUrl);

// Asks GitHub for the newest published release. Drafts and pre-releases are
// excluded by the "latest" endpoint.
internal static class UpdateChecker
{
    private const string LatestReleaseUrl = "https://api.github.com/repos/xenos1337/AirPlayServer/releases/latest";

    // Release builds are stamped with the tag's version by CI. Local builds
    // stay at 0.0.0 and are never offered an update.
    public static Version CurrentVersion { get; } = Trim(Assembly.GetEntryAssembly()?.GetName().Version);

    public static bool IsReleaseBuild => CurrentVersion > new Version(0, 0, 0);

    // Returns the newer release, or null if there is none or GitHub cannot be
    // reached; update checks are never worth an error message.
    public static async Task<UpdateInfo?> CheckAsync()
    {
        try
        {
            using HttpClient client = new() { Timeout = TimeSpan.FromSeconds(15) };
            client.DefaultRequestHeaders.UserAgent.Add(
                new ProductInfoHeaderValue("AirPlayServer", CurrentVersion.ToString()));
            client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));

            using JsonDocument release = JsonDocument.Parse(await client.GetStringAsync(LatestReleaseUrl));
            string tag = release.RootElement.GetProperty("tag_name").GetString() ?? "";
            string page = release.RootElement.GetProperty("html_url").GetString() ?? "";
            if (!Version.TryParse(tag.TrimStart('v', 'V'), out Version? latest) ||
                !page.StartsWith("https://github.com/", StringComparison.Ordinal))
            {
                return null;
            }
            latest = Trim(latest);
            return latest > CurrentVersion ? new UpdateInfo(latest, tag, page) : null;
        }
        catch (Exception e) when (e is HttpRequestException or TaskCanceledException or JsonException
            or InvalidOperationException or KeyNotFoundException)
        {
            return null;
        }
    }

    // Compares major.minor.patch only; assembly versions carry a fourth part.
    private static Version Trim(Version? version)
    {
        return version == null ? new Version(0, 0, 0)
            : new Version(version.Major, version.Minor, Math.Max(version.Build, 0));
    }
}
