using Avalonia.Input;

namespace ConkerLauncher.Services;

/// <summary>
/// A key or mouse button pressed in the launcher, as the game's settings.ini name for it
/// (<c>src/controls.c</c>, the names <c>--dump-settings</c> lists under "keys"). Avalonia's keys
/// come from Windows' virtual-key codes, which is what the game binds, so each maps one to one;
/// left and right Shift, Ctrl and Alt are one key in the game.
/// </summary>
public static class KeyInput
{
    private static readonly Dictionary<Key, string> Named = new()
    {
        [Key.Space] = "space", [Key.Enter] = "enter", [Key.Back] = "backspace", [Key.Tab] = "tab",
        [Key.Escape] = "esc", [Key.LeftShift] = "shift", [Key.RightShift] = "shift",
        [Key.LeftCtrl] = "ctrl", [Key.RightCtrl] = "ctrl", [Key.LeftAlt] = "alt", [Key.RightAlt] = "alt",
        [Key.CapsLock] = "capslock", [Key.Up] = "up", [Key.Down] = "down", [Key.Left] = "left",
        [Key.Right] = "right", [Key.Insert] = "insert", [Key.Delete] = "delete", [Key.Home] = "home",
        [Key.End] = "end", [Key.PageUp] = "pageup", [Key.PageDown] = "pagedown",
        [Key.OemSemicolon] = "semicolon", [Key.OemPlus] = "equals", [Key.OemComma] = "comma",
        [Key.OemMinus] = "minus", [Key.OemPeriod] = "period", [Key.OemQuestion] = "slash",
        [Key.OemTilde] = "backquote", [Key.OemOpenBrackets] = "lbracket", [Key.OemPipe] = "backslash",
        [Key.OemCloseBrackets] = "rbracket", [Key.OemQuotes] = "quote", [Key.Multiply] = "nummul",
        [Key.Add] = "numadd", [Key.Subtract] = "numsub", [Key.Decimal] = "numdot", [Key.Divide] = "numdiv",
        [Key.F4] = "f4", [Key.F5] = "f5", [Key.F6] = "f6", [Key.F7] = "f7", [Key.F8] = "f8",
        [Key.F10] = "f10", [Key.F12] = "f12",
    };

    /// <summary>The game's name for a key, or null when the game can't bind it.</summary>
    public static string? Name(Key key)
    {
        if (key is >= Key.A and <= Key.Z)
        {
            return ((char)('a' + (key - Key.A))).ToString();
        }

        if (key is >= Key.D0 and <= Key.D9)
        {
            return ((char)('0' + (key - Key.D0))).ToString();
        }

        if (key is >= Key.NumPad0 and <= Key.NumPad9)
        {
            return "num" + (key - Key.NumPad0);
        }

        return Named.GetValueOrDefault(key);
    }

    /// <summary>F1-F3, F9 and F11: the game's own keys (menus, trace, full screen).</summary>
    public static bool IsGameOwn(Key key) => key is Key.F1 or Key.F2 or Key.F3 or Key.F9 or Key.F11;

    /// <summary>The game's name for a mouse button, or null.</summary>
    public static string? Name(MouseButton button) => button switch
    {
        MouseButton.Left => "mouse1",
        MouseButton.Right => "mouse2",
        MouseButton.Middle => "mouse3",
        MouseButton.XButton1 => "mouse4",
        MouseButton.XButton2 => "mouse5",
        _ => null,
    };
}
