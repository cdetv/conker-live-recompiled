using System.Net.Http.Headers;
using System.Text.Json;

namespace ConkerLauncher.Services;

/// <summary>
/// A GitHub release: its tag and the first download of the wanted kind, plus its page text
/// (Markdown), title, date and page address for the update and history windows.
/// </summary>
public sealed record ReleaseInfo(string Tag, string? AssetName, string? AssetUrl, long AssetSize)
{
    public string Body { get; init; } = "";

    public string Name { get; init; } = "";

    public DateTimeOffset? Published { get; init; }

    public string? PageUrl { get; init; }
}

/// <summary>Latest releases and downloads from GitHub (the decensor patch and our own updates).</summary>
public static class GitHubReleases
{
    private static readonly HttpClient Http = CreateClient();

    /// <summary>The latest release of <paramref name="repo"/> ("owner/name"), with its first asset ending in one of <paramref name="extensions"/>.</summary>
    public static Task<ReleaseInfo> GetLatestAsync(string repo, string[] extensions, CancellationToken cancel) =>
        GetLatestFromAsync($"https://api.github.com/repos/{repo}/releases/latest", extensions, cancel);

    /// <summary>As <see cref="GetLatestAsync"/>, from any URL answering in GitHub's "latest release" format.</summary>
    public static async Task<ReleaseInfo> GetLatestFromAsync(string url, string[] extensions, CancellationToken cancel)
    {
        using JsonDocument json = await GetJsonAsync(url, cancel);
        return Parse(json.RootElement, extensions);
    }

    /// <summary>The published releases of <paramref name="repo"/>, newest first (GitHub's order; up to 100).</summary>
    public static Task<List<ReleaseInfo>> GetAllAsync(string repo, string[] extensions, CancellationToken cancel) =>
        GetAllFromAsync($"https://api.github.com/repos/{repo}/releases?per_page=100", extensions, cancel);

    /// <summary>As <see cref="GetAllAsync"/>, from any URL answering in GitHub's "list releases" format.</summary>
    public static async Task<List<ReleaseInfo>> GetAllFromAsync(string url, string[] extensions, CancellationToken cancel)
    {
        using JsonDocument json = await GetJsonAsync(url, cancel);
        return [.. json.RootElement.EnumerateArray()
            .Where(r => !Flag(r, "draft") && !Flag(r, "prerelease"))
            .Select(r => Parse(r, extensions))];
    }

    /// <summary>Downloads <paramref name="url"/> to <paramref name="path"/> with progress.</summary>
    public static async Task DownloadAsync(string url, string path, ByteProgress progress, CancellationToken cancel)
    {
        using HttpResponseMessage response = await Http.GetAsync(url, HttpCompletionOption.ResponseHeadersRead, cancel);
        response.EnsureSuccessStatusCode();
        await using Stream body = await response.Content.ReadAsStreamAsync(cancel);
        await using FileStream output = new(path, FileMode.Create, FileAccess.Write, FileShare.None, 1 << 20, useAsync: true);
        byte[] buffer = new byte[1 << 20];
        int n;
        while ((n = await body.ReadAsync(buffer, cancel)) > 0)
        {
            await output.WriteAsync(buffer.AsMemory(0, n), cancel);
            progress.Add(n);
        }
    }

    private static async Task<JsonDocument> GetJsonAsync(string url, CancellationToken cancel)
    {
        using HttpResponseMessage response = await Http.GetAsync(url, cancel);
        response.EnsureSuccessStatusCode();
        await using Stream body = await response.Content.ReadAsStreamAsync(cancel);
        return await JsonDocument.ParseAsync(body, cancellationToken: cancel);
    }

    private static ReleaseInfo Parse(JsonElement root, string[] extensions)
    {
        string tag = root.GetProperty("tag_name").GetString() ?? "";
        ReleaseInfo release = new(tag, null, null, 0);
        foreach (JsonElement asset in root.GetProperty("assets").EnumerateArray())
        {
            string name = asset.GetProperty("name").GetString() ?? "";
            if (extensions.Any(x => name.EndsWith(x, StringComparison.OrdinalIgnoreCase)))
            {
                release = new ReleaseInfo(tag, name, asset.GetProperty("browser_download_url").GetString(), asset.GetProperty("size").GetInt64());
                break;
            }
        }

        // The rest is only shown: a feed without it (the development override) still updates.
        return release with
        {
            Body = Text(root, "body") ?? "",
            Name = Text(root, "name") ?? "",
            Published = Text(root, "published_at") is { } date && DateTimeOffset.TryParse(date,
                System.Globalization.CultureInfo.InvariantCulture, System.Globalization.DateTimeStyles.AssumeUniversal, out DateTimeOffset d) ? d : null,
            PageUrl = Text(root, "html_url"),
        };
    }

    private static string? Text(JsonElement root, string name) =>
        root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.String ? value.GetString() : null;

    private static bool Flag(JsonElement root, string name) =>
        root.TryGetProperty(name, out JsonElement value) && value.ValueKind == JsonValueKind.True;

    private static HttpClient CreateClient()
    {
        HttpClient client = new() { Timeout = TimeSpan.FromMinutes(30) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("conker-launcher", "1.0"));
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return client;
    }
}
