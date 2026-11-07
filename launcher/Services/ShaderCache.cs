using System.Text.RegularExpressions;

namespace ConkerLauncher.Services;

/// <summary>What the last "Precompile Shaders" run wrote (shader_cache\precompiled.ini).</summary>
/// <param name="Version">The game version that ran it (the cache keys change with the game's shader code).</param>
/// <param name="Date">When it ended, as the game wrote it (local time, yyyy-MM-dd HH:mm).</param>
/// <param name="Finished">It ran to the end (not stopped with Esc or the window closed).</param>
/// <param name="ResumeWhere">A stopped run: where the next one continues ("level 12 of 47", "chapter 3 of 9"); null otherwise.</param>
public sealed record PrecompileResult(Version? Version, string Date, bool Finished, int Levels, int Chapters, int Seconds,
                                      string? ResumeWhere);

/// <summary>
/// The game's shader cache in the user files (SPEC 9.3 Files tab, to-do 25): Precompile Shaders
/// starts <c>conker.exe --precompile-shaders</c>, which runs every level and chapter behind a
/// progress screen to fill it, then writes <c>precompiled.ini</c> beside it. A stopped run
/// continues where it was the next time (same version); Delete Shaders empties the cache.
/// </summary>
public static partial class ShaderCache
{
    public static string Folder => Path.Combine(SaveFolder.Root, "shader_cache");

    public static string ResultFile => Path.Combine(Folder, "precompiled.ini");

    /// <summary>A cache file's name, as the game writes them: 16 hex digits, .vso or .cso.</summary>
    [GeneratedRegex(@"^[0-9A-Fa-f]{16}\.(vso|cso)$")]
    private static partial Regex CacheName();

    /// <summary>The last run's result; null when none ran (or the file can't be read).</summary>
    public static PrecompileResult? ReadResult()
    {
        try
        {
            if (!File.Exists(ResultFile))
            {
                return null;
            }

            Dictionary<string, string> values = new(StringComparer.OrdinalIgnoreCase);
            foreach (string line in File.ReadLines(ResultFile))
            {
                int eq = line.IndexOf('=');
                if (eq > 0 && !line.TrimStart().StartsWith(';'))
                {
                    values[line[..eq].Trim()] = line[(eq + 1)..].Trim();
                }
            }

            Version? version = values.TryGetValue("version", out string? v) && Version.TryParse(v, out Version? parsed)
                ? parsed : null;
            bool finished = values.GetValueOrDefault("result", "").StartsWith("done", StringComparison.OrdinalIgnoreCase);
            string? where = finished ? null : values.GetValueOrDefault("resume") switch
            {
                "levels" => $"level {Number(values, "resume_levels")} of 47",
                "chapters" => Number(values, "resume_chapter") >= 9
                    ? "the very end"
                    : $"chapter {Number(values, "resume_chapter") + 1} of 9",
                _ => null,
            };
            return new PrecompileResult(
                version,
                values.GetValueOrDefault("date", ""),
                finished,
                Number(values, "levels"),
                Number(values, "chapters"),
                Number(values, "seconds"),
                where);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return null;
        }
    }

    /// <summary>Any compiled shader or a result file in the cache.</summary>
    public static bool HasFiles()
    {
        try
        {
            return Directory.Exists(Folder)
                   && (File.Exists(ResultFile)
                       || Directory.EnumerateFiles(Folder).Any(f => CacheName().IsMatch(Path.GetFileName(f))));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return false;
        }
    }

    /// <summary>
    /// Delete Shaders: every .vso / .cso the game wrote and precompiled.ini (nothing else in
    /// the folder). Returns how many shader files went; throws if one could not be deleted.
    /// </summary>
    public static int Delete()
    {
        if (!Directory.Exists(Folder))
        {
            return 0;
        }

        int n = 0;
        foreach (string file in Directory.EnumerateFiles(Folder).ToList())
        {
            if (CacheName().IsMatch(Path.GetFileName(file)))
            {
                File.Delete(file);
                n++;
            }
        }

        File.Delete(ResultFile);
        return n;
    }

    private static int Number(Dictionary<string, string> values, string key) =>
        values.TryGetValue(key, out string? s) && int.TryParse(s, out int n) ? n : 0;
}
