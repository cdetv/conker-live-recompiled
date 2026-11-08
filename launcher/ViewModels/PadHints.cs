using Avalonia;
using Avalonia.Media;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>One drawn button in the hints bar: a round face button or a small label key.</summary>
/// <param name="Text">Letter or label (none for a PlayStation shape).</param>
/// <param name="Shape">A PlayStation face button's shape, drawn in <paramref name="Ink"/>.</param>
public sealed record PadGlyph(string? Text, IBrush Fill, IBrush Ink, bool Round, Geometry? Shape)
{
    public bool HasShape => Shape is not null;

    /// <summary>A drawn shape's fill (the D-pad arrows); PlayStation shapes are outlines.</summary>
    public IBrush? ShapeFill { get; init; }

    /// <summary>Face buttons round, labels (LB, Start ...) a rounded box.</summary>
    public CornerRadius Corners => new(Round ? 10 : 4);
}

/// <summary>An entry of the hints bar: its buttons and what they do.</summary>
public sealed record PadHint(IReadOnlyList<PadGlyph> Glyphs, string Label);

/// <summary>What has the pad now, which decides the hints.</summary>
public enum PadHintState
{
    Page,
    Adjusting,      // a slider: Left / Right change it
    Choosing,       // an open dropdown
    PopUp,          // a pop-up or an inline question
    KeyCapture,     // "Press a key"
}

/// <summary>
/// The button hints bar (to-do 121 part 1f): Xbox letters or PlayStation shapes by the last
/// pad's type. Our own simple drawings (coloured circles, plain shapes, labels), no logos.
/// </summary>
public static class PadHints
{
    private static readonly IBrush Dark = new SolidColorBrush(Color.Parse("#FF101410"));
    private static readonly IBrush KeyFill = new SolidColorBrush(Color.Parse("#FF3A403A"));
    private static readonly IBrush KeyInk = new SolidColorBrush(Color.Parse("#FFE8ECE8"));
    private static readonly IBrush PsFill = new SolidColorBrush(Color.Parse("#FF262A26"));

    // Xbox: the letter on its colour. PlayStation: the shape in its colour on a dark circle.
    private static readonly PadGlyph XboxA = Face("A", "#FF5DBB1E");
    private static readonly PadGlyph XboxB = Face("B", "#FFE04848");
    private static readonly PadGlyph XboxX = Face("X", "#FF3C8CE0");
    private static readonly PadGlyph PsCross = Shape("M0,0 L10,10 M10,0 L0,10", "#FF7FA8E8");
    private static readonly PadGlyph PsCircle = Shape("M5,0 A5,5 0 1 1 4.99,0 Z", "#FFE86868");
    private static readonly PadGlyph PsSquare = Shape("M0.5,0.5 L9.5,0.5 L9.5,9.5 L0.5,9.5 Z", "#FFE090D0");
    private static readonly PadGlyph LeftRight = new(null, KeyFill, KeyInk, false, Geometry.Parse("M0,5 L5,1 L5,9 Z M16,5 L11,1 L11,9 Z"))
    {
        ShapeFill = KeyInk,
    };

    public static IReadOnlyList<PadHint> For(PadHintState state, PadKind kind)
    {
        bool ps = kind == PadKind.PlayStation;
        PadGlyph a = ps ? PsCross : XboxA;
        PadGlyph b = ps ? PsCircle : XboxB;
        PadGlyph x = ps ? PsSquare : XboxX;
        return state switch
        {
            PadHintState.Adjusting => [new([LeftRight], "Change"), new([a, b], "Done")],
            PadHintState.Choosing => [new([a], "Choose"), new([b], "Close")],
            PadHintState.PopUp => [new([a], "Select"), new([b], "Back")],
            PadHintState.KeyCapture => [new([x], "No key"), new([b], "Cancel")],
            _ =>
            [
                new([Key(ps ? "L2" : "LT"), Key(ps ? "R2" : "RT")], "Tabs"),
                new([Key(ps ? "L1" : "LB"), Key(ps ? "R1" : "RB")], "Sections"),
                new([a], "Select"),
                new([Key(ps ? "Options" : "Start")], "Play"),
                new([Key(ps ? "Share" : "Back")], "Quit"),
            ],
        };
    }

    /// <summary>
    /// The file browser (to-do 122): pages, Places &lt;-&gt; list, A by the focused row (Open /
    /// Choose / Select), B up, X all files or fewer, Back / Share closes it.
    /// </summary>
    public static IReadOnlyList<PadHint> ForBrowser(PadKind kind, string aLabel, bool showingAll)
    {
        bool ps = kind == PadKind.PlayStation;
        return
        [
            new([Key(ps ? "L2" : "LT"), Key(ps ? "R2" : "RT")], "Page"),
            new([Key(ps ? "L1" : "LB"), Key(ps ? "R1" : "RB")], "Places"),
            new([ps ? PsCross : XboxA], aLabel),
            new([ps ? PsCircle : XboxB], "Up"),
            new([ps ? PsSquare : XboxX], showingAll ? "Fewer files" : "All files"),
            new([Key(ps ? "Share" : "Back")], "Close"),
        ];
    }

    private static PadGlyph Face(string letter, string colour) =>
        new(letter, new SolidColorBrush(Color.Parse(colour)), Dark, true, null);

    private static PadGlyph Shape(string path, string colour) =>
        new(null, PsFill, new SolidColorBrush(Color.Parse(colour)), true, Geometry.Parse(path));

    private static PadGlyph Key(string label) => new(label, KeyFill, KeyInk, false, null);
}
