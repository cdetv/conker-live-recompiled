using System.Text;
using System.Text.RegularExpressions;

namespace ConkerLauncher.Services;

/// <summary>A run of text in a release block, bold or not.</summary>
public sealed record TextSpan(string Text, bool Bold);

/// <summary>A heading, paragraph or bullet (<see cref="Level"/> 1 = top-level bullet, 0 = not one).</summary>
public sealed record TextBlockInfo(List<TextSpan> Spans, int Level, bool Heading);

/// <summary>
/// The small part of GitHub Markdown our release pages use (SPEC 9.3), for the update and
/// Version History windows: headings, paragraphs, nested bullets and <c>**bold**</c>. Links
/// show their text, <c>`code`</c> its text; comments, images and HTML lines are left out.
/// </summary>
public static partial class ReleaseText
{
    public static List<TextBlockInfo> Parse(string markdown)
    {
        string text = Comment().Replace(markdown.Replace("\r\n", "\n"), "");
        List<TextBlockInfo> blocks = [];
        StringBuilder current = new();
        int level = 0;
        bool heading = false;

        void Flush()
        {
            if (current.Length > 0)
            {
                blocks.Add(new TextBlockInfo(Spans(current.ToString()), level, heading));
            }

            current.Clear();
            level = 0;
            heading = false;
        }

        foreach (string raw in text.Split('\n'))
        {
            string line = raw.TrimEnd();
            string trimmed = line.TrimStart();
            if (trimmed.Length == 0 || trimmed.StartsWith('<') || trimmed.StartsWith("![", StringComparison.Ordinal)
                || trimmed.Trim('-', '*', '_', ' ').Length == 0)
            {
                // Blank, HTML, an image or a rule: ends the block.
                Flush();
                continue;
            }

            Match bullet = Bullet().Match(line);
            if (bullet.Success)
            {
                Flush();
                level = 1 + (bullet.Groups[1].Value.Replace("\t", "    ").Length / 2);
                current.Append(bullet.Groups[2].Value);
            }
            else if (trimmed.StartsWith('#'))
            {
                Flush();
                heading = true;
                current.Append(trimmed.TrimStart('#').Trim());
                Flush();
            }
            else
            {
                // A paragraph's next line, or a bullet's continued text.
                if (current.Length > 0)
                {
                    current.Append(' ');
                }

                current.Append(trimmed);
            }
        }

        Flush();
        return blocks;
    }

    /// <summary>The text with <c>**bold**</c> split out and links, code and escapes made plain.</summary>
    private static List<TextSpan> Spans(string text)
    {
        text = Image().Replace(text, "");
        text = Link().Replace(text, "$1");
        text = Escape().Replace(text.Replace("`", "", StringComparison.Ordinal), "$1");
        List<TextSpan> spans = [];
        string[] parts = text.Split("**");
        for (int i = 0; i < parts.Length; i++)
        {
            if (parts[i].Length > 0)
            {
                // An unclosed ** leaves its last part plain.
                spans.Add(new TextSpan(parts[i], i % 2 == 1 && i < parts.Length - (parts.Length % 2 == 0 ? 1 : 0)));
            }
        }

        return spans;
    }

    [GeneratedRegex(@"<!--.*?-->", RegexOptions.Singleline)]
    private static partial Regex Comment();

    [GeneratedRegex(@"^(\s*)[-*+]\s+(.*)$")]
    private static partial Regex Bullet();

    [GeneratedRegex(@"!\[[^\]]*\]\([^)]*\)")]
    private static partial Regex Image();

    [GeneratedRegex(@"\[([^\]]*)\]\([^)]*\)")]
    private static partial Regex Link();

    // A Markdown escape (\* and the like); C:\Program Files keeps its backslash.
    [GeneratedRegex(@"\\([\\`*_{}\[\]()#+\-.!|])")]
    private static partial Regex Escape();
}
