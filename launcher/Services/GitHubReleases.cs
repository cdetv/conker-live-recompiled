using System.Net.Http.Headers;
using System.Text.Json;

namespace ConkerLauncher.Services;

/// <summary>A GitHub release: its tag and the first download of the wanted kind.</summary>
public sealed record ReleaseInfo(string Tag, string? AssetName, string? AssetUrl, long AssetSize);

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
        using HttpResponseMessage response = await Http.GetAsync(url, cancel);
        response.EnsureSuccessStatusCode();
        await using Stream body = await response.Content.ReadAsStreamAsync(cancel);
        using JsonDocument json = await JsonDocument.ParseAsync(body, cancellationToken: cancel);
        JsonElement root = json.RootElement;
        string tag = root.GetProperty("tag_name").GetString() ?? "";
        foreach (JsonElement asset in root.GetProperty("assets").EnumerateArray())
        {
            string name = asset.GetProperty("name").GetString() ?? "";
            if (extensions.Any(x => name.EndsWith(x, StringComparison.OrdinalIgnoreCase)))
            {
                return new ReleaseInfo(tag, name, asset.GetProperty("browser_download_url").GetString(), asset.GetProperty("size").GetInt64());
            }
        }

        return new ReleaseInfo(tag, null, null, 0);
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

    private static HttpClient CreateClient()
    {
        HttpClient client = new() { Timeout = TimeSpan.FromMinutes(30) };
        client.DefaultRequestHeaders.UserAgent.Add(new ProductInfoHeaderValue("conker-launcher", "1.0"));
        client.DefaultRequestHeaders.Accept.Add(new MediaTypeWithQualityHeaderValue("application/vnd.github+json"));
        return client;
    }
}
