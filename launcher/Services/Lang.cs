using System.Globalization;
using System.Text;
using System.Text.RegularExpressions;
using Avalonia;
using Avalonia.Data;

namespace ConkerLauncher.Services;

/// <summary>
/// The launcher in the game's languages (to-do 289, SPEC 9.3 "Languages"): the same
/// <c>lang\&lt;code&gt;.txt</c> files as the game's own menus (src/ui_text.cpp), English the key, a
/// missing file or line shows English. It follows the game's Menu language (settings.ini
/// <c>ui_language</c>; auto = the Text language <c>language</c>), at once when it changes: the
/// Settings tab calls <see cref="Follow"/> with every change and every reload (a change made in
/// F2 shows when the game closes). Texts: <c>T("...")</c> (now) and <c>S("...")</c> (kept as
/// "what to say", drawn again after a switch) in C#, <c>{l:T '...'}</c> in .axaml; numbered
/// slots <c>{0}</c> for values, so a translation can put them in its own order.
/// scripts/check_lang.py finds every one and writes the lang files.
/// </summary>
public static partial class Lang
{
    private static readonly string[] JapaneseFonts = ["YuGothM.ttc", "meiryo.ttc", "msgothic.ttc"];

    private static readonly (string Name, string Code)[] Languages =
        [("english", ""), ("french", "fr"), ("german", "de"), ("italian", "it"), ("spanish", "es"), ("japanese", "ja")];

    private static Dictionary<string, string> table = new(StringComparer.Ordinal);
    private static Dictionary<string, string> menuCache = new(StringComparer.Ordinal);
    private static Dictionary<string, string> helpTable = new(StringComparer.Ordinal);
    private static List<(Regex English, string Shown)> menuFormats = [];
    private static string? root;
    private static bool loaded;

    /// <summary>The language shown: "" = English, else de, fr, es, it or ja.</summary>
    public static string Code { get; private set; } = "";

    /// <summary>Raised on the UI thread after a switch: every view model draws its text again.</summary>
    public static event EventHandler? Changed;

    /// <summary>Numbers as the language writes them (1.234,5 in German); the format of every <c>T</c> with values.</summary>
    public static NumberFormatInfo Numbers { get; private set; } = NumberFormatInfo.InvariantInfo;

    /// <summary>The launcher's folder, which holds lang\ next to conker.exe (set once at start).</summary>
    public static void Init(string launcherFolder) => root = launcherFolder;

    /// <summary>
    /// Follows the Menu language: <paramref name="uiLanguage"/> (settings.ini <c>ui_language</c>;
    /// empty or auto = <paramref name="textLanguage"/>, the Text language; empty = English).
    /// </summary>
    public static void Follow(string? uiLanguage, string? textLanguage)
    {
        string name = string.IsNullOrWhiteSpace(uiLanguage) || uiLanguage.Trim().Equals("auto", StringComparison.OrdinalIgnoreCase)
            ? textLanguage ?? ""
            : uiLanguage;
        string code = Languages.FirstOrDefault(l => l.Name.Equals(name.Trim(), StringComparison.OrdinalIgnoreCase)).Code ?? "";
        // As the game (src/overlay.cpp add_system_fonts): Japanese needs one of Windows' Japanese fonts, else English.
        if (code == "ja" && !JapaneseFont())
        {
            code = "";
        }

        if (loaded && code == Code)
        {
            return;
        }

        loaded = true;
        table = Load(code);
        menuCache = new(StringComparer.Ordinal);
        menuFormats = MenuFormats(table);
        // settings.ini's comments (lines with breaks): looked up by the one line --dump-settings makes of them.
        helpTable = table.Where(l => l.Key.Contains('\n', StringComparison.Ordinal))
            .GroupBy(l => l.Key.Replace('\n', ' '))
            .ToDictionary(g => g.Key, g => g.First().Value.Replace('\n', ' '), StringComparer.Ordinal);
        Code = code;
        Numbers = NumbersFor(code);
        Changed?.Invoke(null, EventArgs.Empty);
        LangText.Refresh();
    }

    /// <summary>Follows what settings.ini says now (at start, before the Settings tab has read it).</summary>
    public static void FollowSettingsFile()
    {
        try
        {
            Dictionary<string, string> values = SettingsFile.Read(SaveFolder.SettingsIni);
            Follow(values.GetValueOrDefault("ui_language"), values.GetValueOrDefault("language"));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            Follow(null, null);
        }
    }

    /// <summary>The text in the language shown: the translation of <paramref name="en"/>, or the English.</summary>
    public static string T(string en) => table.TryGetValue(en, out string? shown) ? shown : en;

    /// <summary>
    /// <see cref="T(string)"/> with values in its <c>{0}</c>, <c>{1}</c> slots, written as the language
    /// writes numbers. A translation whose slots don't work shows the English.
    /// </summary>
    public static string T(string en, params object?[] args)
    {
        try
        {
            return string.Format(Numbers, T(en), args);
        }
        catch (FormatException)
        {
            return string.Format(Numbers, en, args);
        }
    }

    /// <summary>
    /// A text of the game's F2 / F3 menus as <c>conker.exe --dump-settings</c> prints it (always
    /// English; to-do 289 step 3), in the game's own translation: its line in lang\ (the game marks
    /// them <c>N_()</c> / <c>T()</c>); a chord or button combo ("Back + D-pad Up") part by part; or a
    /// text the game filled into one of its lines with printf slots ("Reset all single player
    /// controls" = "Reset all %s controls" with "single player"), matched back to that line with
    /// each <c>%s</c> said too (numbers stay). Anything else stays as printed (key and button
    /// names, units, numbers, languages' own names).
    /// </summary>
    public static string Menu(string? en)
    {
        if (string.IsNullOrEmpty(en) || Code.Length == 0)
        {
            return en ?? "";
        }

        if (menuCache.TryGetValue(en, out string? cached))
        {
            return cached;
        }

        string shown = MenuText(en);
        menuCache[en] = shown;
        return shown;
    }

    /// <summary>
    /// An option's settings.ini comment as <c>--dump-settings</c> gives it (<c>help</c>: one line,
    /// the file's breaks made spaces), the launcher's tooltip for a row the menu has none for:
    /// its translation (the game marks those comments <c>N_()</c> in settings.c; step 4), or the English.
    /// </summary>
    public static string Help(string en) => helpTable.TryGetValue(en, out string? shown) ? shown : en;

    /// <summary>
    /// A controller button or a combo of them as the game names it ("Back + D-pad Up", "Left stick up",
    /// "Right trigger"): the parts of more than one word are said; a one-word part is a button's own name
    /// (A, LB, Start, Back, Black), which stays ("Back" is also our "go back" line, not the button).
    /// </summary>
    public static string Button(string? en)
    {
        if (string.IsNullOrEmpty(en))
        {
            return en ?? "";
        }

        // A whole line the game has ("D-pad up + right stick up", "Left stick up") first.
        if (en.Contains(' ', StringComparison.Ordinal) && table.TryGetValue(en, out string? whole))
        {
            return whole;
        }

        return string.Join(" + ", en.Split(" + ").Select(p => p.Contains(' ', StringComparison.Ordinal) ? Menu(p) : p));
    }

    private static string MenuText(string en)
    {
        if (table.TryGetValue(en, out string? exact))
        {
            return exact;
        }

        string trimmed = en.Trim();
        if (trimmed.Length != en.Length && trimmed.Length > 0)
        {
            int lead = en.Length - en.TrimStart().Length;
            return en[..lead] + MenuText(trimmed) + en[(lead + trimmed.Length)..];
        }

        // A combo: "D-pad Up", "right stick up" are said; one-word parts are button names (A, LB,
        // Back, Start), which stay: "Back" is also a line of ours ("go back"), not the button.
        if (en.Contains(" + ", StringComparison.Ordinal))
        {
            return string.Join(" + ", en.Split(" + ").Select(p => p.Contains(' ', StringComparison.Ordinal) ? table.GetValueOrDefault(p, p) : p));
        }

        foreach ((Regex english, string format) in menuFormats)
        {
            Match m = english.Match(en);
            if (!m.Success)
            {
                continue;
            }

            // The slots in the translation's order are the English's (check_lang checks it).
            int i = 0;
            string result = Printf().Replace(format, s =>
            {
                if (s.Value == "%%")
                {
                    return "%";
                }

                string value = ++i < m.Groups.Count ? m.Groups[i].Value : "";
                return s.Value.EndsWith('s') ? MenuText(value) : value;
            });
            return i == m.Groups.Count - 1 ? result : en;
        }

        return en;
    }

    /// <summary>The lines with printf slots as patterns over the English they make, the longest first.</summary>
    private static List<(Regex, string)> MenuFormats(Dictionary<string, string> lines)
    {
        var list = new List<(Regex, string)>();
        foreach ((string key, string shown) in lines.OrderByDescending(l => l.Key.Length))
        {
            if (!key.Contains('%', StringComparison.Ordinal) || !Printf().Matches(key).Any(s => s.Value != "%%"))
            {
                continue;
            }

            var pattern = new StringBuilder("^");
            int at = 0;
            foreach (Match s in Printf().Matches(key))
            {
                pattern.Append(Regex.Escape(key[at..s.Index]));
                pattern.Append(s.Value == "%%" ? "%" : s.Value.EndsWith('s') ? "(.+?)" : "([-+]?[0-9][0-9.,]*)");
                at = s.Index + s.Length;
            }

            pattern.Append(Regex.Escape(key[at..])).Append('$');
            list.Add((new Regex(pattern.ToString(), RegexOptions.CultureInvariant | RegexOptions.Singleline), shown));
        }

        return list;
    }

    /// <summary>A printf slot as the game's lines have them (check_lang's SPEC), or %%.</summary>
    [GeneratedRegex(@"%%|%[-+#0]*(?:\d+|\*)?(?:\.(?:\d+|\*))?(?:hh|h|ll|l|L|z|j|t|I64|I32)?[diouxXeEfgGaAcspn]")]
    private static partial Regex Printf();

    /// <summary>Text kept as "what to say" (a result, an error): said again in the new language after a switch.</summary>
    public static Said S(string en, params object?[] args) => new(() => T(en, args));

    /// <summary>
    /// An exception's reason as a value for <see cref="S"/>: the launcher's own words
    /// (<see cref="ImportException"/>, <see cref="SaidIOException"/>) follow the language; a reason
    /// from Windows or .NET stays as it came.
    /// </summary>
    public static Said Why(Exception e) => new(() => e.Message);

    /// <summary>A date and time as the language writes them (the day first, or the year in Japanese).</summary>
    public static string Date(DateTime t) => t.ToString(Code switch
    {
        "de" => "dd.MM.yyyy, HH:mm",
        "fr" => "dd/MM/yyyy HH:mm",
        "es" or "it" => "dd/MM/yyyy, HH:mm",
        "ja" => "yyyy/MM/dd HH:mm",
        _ => "d MMM yyyy, HH:mm",
    }, CultureInfo.InvariantCulture);

    /// <summary>A day as the language writes it: "3 November 2026" in English, numbers in the others (no month names without culture data).</summary>
    public static string Day(DateTime t) => t.ToString(Code switch
    {
        "de" => "dd.MM.yyyy",
        "fr" or "es" or "it" => "dd/MM/yyyy",
        "ja" => "yyyy/MM/dd",
        _ => "d MMMM yyyy",
    }, CultureInfo.InvariantCulture);

    /// <summary>
    /// Yu Gothic, Meiryo or MS Gothic in Windows' Fonts folder, the fonts the game draws Japanese with
    /// (under Proton the prefix links a stand-in msgothic.ttc). <c>CONKER_LAUNCHER_FONTS</c>
    /// (development only): another folder to look in (an empty one = none).
    /// </summary>
    private static bool JapaneseFont()
    {
        string fonts = Environment.GetEnvironmentVariable("CONKER_LAUNCHER_FONTS") is { Length: > 0 } dir
            ? dir
            : Environment.GetFolderPath(Environment.SpecialFolder.Fonts);
        try
        {
            return JapaneseFonts.Any(f => File.Exists(Path.Combine(fonts, f)));
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException or ArgumentException)
        {
            return false;
        }
    }

    private static NumberFormatInfo NumbersFor(string code)
    {
        if (code is "" or "ja")
        {
            return NumberFormatInfo.InvariantInfo;
        }

        // InvariantGlobalization: no culture data in the launcher, so the separators are set here.
        var n = (NumberFormatInfo)NumberFormatInfo.InvariantInfo.Clone();
        n.NumberDecimalSeparator = ",";
        n.NumberGroupSeparator = code == "fr" ? " " : ".";
        n.PercentDecimalSeparator = n.NumberDecimalSeparator;
        n.PercentGroupSeparator = n.NumberGroupSeparator;
        return NumberFormatInfo.ReadOnly(n);
    }

    /// <summary>lang\&lt;code&gt;.txt: "en: &lt;English&gt;" then "&lt;code&gt;: &lt;text&gt;", as src/ui_text.cpp reads it.</summary>
    private static Dictionary<string, string> Load(string code)
    {
        var map = new Dictionary<string, string>(StringComparer.Ordinal);
        if (code.Length == 0 || root is null)
        {
            return map;
        }

        string[] lines;
        try
        {
            lines = File.ReadAllLines(Path.Combine(root, "lang", code + ".txt"), Encoding.UTF8);
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            return map;                 // no file: English
        }

        string? key = null;
        string prefix = code + ":";
        foreach (string line in lines)
        {
            if (line.StartsWith("en: ", StringComparison.Ordinal))
            {
                key = Unescape(line[4..].TrimStart(' '));
            }
            else if (key is not null && line.StartsWith(prefix, StringComparison.Ordinal))
            {
                string text = line[prefix.Length..];
                if (text.StartsWith(' '))
                {
                    text = text[1..];
                }

                if (text.Length > 0)
                {
                    map[key] = Unescape(text);
                }

                key = null;
            }
        }

        return map;
    }

    /// <summary>"\n" and "\\" in a line of the file.</summary>
    private static string Unescape(string s)
    {
        if (!s.Contains('\\', StringComparison.Ordinal))
        {
            return s;
        }

        var b = new StringBuilder(s.Length);
        for (int i = 0; i < s.Length; i++)
        {
            if (s[i] == '\\' && i + 1 < s.Length && s[i + 1] is 'n' or '\\')
            {
                b.Append(s[i + 1] == 'n' ? '\n' : '\\');
                i++;
            }
            else
            {
                b.Append(s[i]);
            }
        }

        return b.ToString();
    }
}

/// <summary>
/// Text kept as "what to say": the English and its values, put into words each time it is shown, so
/// a result or an error already on screen changes language with everything else. A value that is
/// itself a <see cref="Said"/> is said in the same language.
/// </summary>
public sealed class Said(Func<string> say)
{
    /// <summary>Words that are already words (a message from Windows, a name): shown as they are.</summary>
    public static Said As(string text) => new(() => text);

    /// <summary>A text of the game's menus from <c>--dump-settings</c>, in the game's translation (<see cref="Lang.Menu"/>).</summary>
    public static Said Menu(string text) => new(() => Lang.Menu(text));

    /// <summary>A controller button or combo from <c>--dump-settings</c> (<see cref="Lang.Button"/>).</summary>
    public static Said Button(string text) => new(() => Lang.Button(text));

    /// <summary>This, then <paramref name="next"/> as a sentence of its own (none: this alone); Japanese puts no space between.</summary>
    public Said Then(Said? next) => next is null ? this : new(() => say() + (Lang.Code == "ja" ? "" : " ") + next);

    public override string ToString() => say();
}

/// <summary>An <see cref="IOException"/> in the launcher's own words, said in the language shown.</summary>
public sealed class SaidIOException(Said text, Exception? inner = null) : IOException(null, inner)
{
    public override string Message => text.ToString();
}

/// <summary>
/// <c>{l:T 'English'}</c> in .axaml (to-do 289): the text in the language shown, changing with it.
/// In the quotes, <c>\\</c> is a backslash (<c>textures\\dump</c>): one stays, whether or not the
/// XAML reader already took the escape away.
/// </summary>
public sealed class TExtension(string text)
{
    public string Text { get; } = text;

    public BindingBase ProvideValue(IServiceProvider services) =>
        new LangText(Text.Replace("\\\\", "\\", StringComparison.Ordinal)).ToBinding();
}

/// <summary>
/// One <c>{l:T}</c> text as an observable: its words now, and again after every switch. The
/// registry holds the controls' bindings weakly (the subscription a binding keeps holds them),
/// so a control that is gone (a list row) is let go.
/// </summary>
internal sealed class LangText(string en) : IObservable<string>
{
    private static readonly List<(WeakReference<IObserver<string>> Observer, string En)> Live = [];

    public IDisposable Subscribe(IObserver<string> observer)
    {
        observer.OnNext(Lang.T(en));
        Live.Add((new WeakReference<IObserver<string>>(observer), en));
        return new Subscription(observer);
    }

    /// <summary>After a switch (UI thread): every text still shown says its words again.</summary>
    internal static void Refresh()
    {
        Live.RemoveAll(l => !l.Observer.TryGetTarget(out _));
        foreach ((WeakReference<IObserver<string>> weak, string en) in Live.ToArray())
        {
            if (weak.TryGetTarget(out IObserver<string>? observer))
            {
                observer.OnNext(Lang.T(en));
            }
        }
    }

    /// <summary>Kept by the binding: holds its observer alive; disposing it ends the updates.</summary>
    private sealed class Subscription(IObserver<string> observer) : IDisposable
    {
        private IObserver<string>? held = observer;

        public void Dispose()
        {
            if (held is { } o)
            {
                Live.RemoveAll(l => l.Observer.TryGetTarget(out IObserver<string>? x) && ReferenceEquals(x, o));
                held = null;
            }
        }
    }
}
