using System.Collections.ObjectModel;
using CommunityToolkit.Mvvm.ComponentModel;
using ConkerLauncher.Services;

namespace ConkerLauncher.ViewModels;

/// <summary>One row of the pack list: a folder in textures\packs, on (ticked) or off.</summary>
public sealed partial class TexturePackItem : ObservableObject
{
    private readonly TexturePacksViewModel owner;

    public TexturePackItem(TexturePacksViewModel owner, string name, bool on)
    {
        this.owner = owner;
        Name = name;
        On = on;
    }

    [ObservableProperty]
    public partial string Name { get; set; }

    [ObservableProperty]
    public partial bool On { get; set; }

    /// <summary>Held to move (a drag, or A held on it): the row stands out.</summary>
    [ObservableProperty]
    public partial bool IsHeld { get; set; }

    // A tick or untick from the check box (a click, Space, or A let go without a move).
    partial void OnOnChanged(bool value) => owner.Ticked(this);
}

/// <summary>
/// The Files tab's texture pack list (to-do 288 part 2d; SPEC 9.3 "Texture packs"): every folder in
/// textures\packs, a check box each, the top one first (it wins where two packs change the same
/// texture), saved as settings.ini's <c>[texture_packs]</c> the moment it changes, which the game
/// reads at its start. A move swaps two rows' contents (name, tick, held), never the rows
/// themselves, so a dragged or pad-held row's control (its focus, the pointer) stays put while the
/// pack goes up or down; the move is saved once, when the row is let go (<see cref="EndMove"/>).
/// Locked while the game runs: its F2 menu writes settings.ini then (Extras > Texture Packs).
/// </summary>
public sealed partial class TexturePacksViewModel : ObservableObject
{
    private readonly GameFolder folder;
    private readonly Func<bool> isGameRunning;
    private readonly Func<SettingsSchema?> schema;
    private bool quiet;                 // rows being filled or swapped: no saves

    public TexturePacksViewModel(GameFolder folder, Func<bool> isGameRunning, Func<SettingsSchema?> schema)
    {
        this.folder = folder;
        this.isGameRunning = isGameRunning;
        this.schema = schema;
        // A language switch (to-do 289): every text here said again.
        Lang.Changed += (_, _) => OnPropertyChanged(string.Empty);
    }

    public ObservableCollection<TexturePackItem> Packs { get; } = [];

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Note), nameof(CanChange))]
    public partial bool Locked { get; set; }

    [ObservableProperty]
    [NotifyPropertyChangedFor(nameof(Error))]
    public partial Said? ErrorSaid { get; set; }

    public string? Error => ErrorSaid?.ToString();

    /// <summary>The list can be changed: not while the game runs.</summary>
    public bool CanChange => !Locked;

    public bool HasPacks => Packs.Count > 0;

    /// <summary>The line above the list.</summary>
    public string Note => Locked
        ? T("The game is running: change the packs in its F2 menu (Extras > Texture Packs).")
        : Packs.Count == 0
            ? T("No packs yet: put a pack's folder in textures\\packs (Packs Folder opens it).")
            : T("Ticked packs are on; the top one wins where two packs change the same texture. Drag a pack to "
                + "move it, or with a controller hold A and press up or down. Saved at once; the game reads them "
                + "when it starts (in the game: F2 > Extras > Texture Packs).");

    /// <summary>A row is being moved (dragged or held with A): the list isn't read again meanwhile.</summary>
    public bool Moving => Packs.Any(p => p.IsHeld);

    /// <summary>Reads the folders and settings.ini again (not while a row is being moved).</summary>
    public void Refresh()
    {
        Locked = isGameRunning();
        if (Moving)
        {
            return;
        }

        List<(string Name, bool On)> list;
        try
        {
            list = TexturePacks.List(TexturePacks.Found(folder), SettingsFile.ReadPacks(SaveFolder.SettingsIni));
        }
        catch (IOException e)
        {
            ErrorSaid = S("settings.ini couldn't be read: {0}", e.Message);
            return;
        }

        // The same list: the rows stay (a refresh on every window activation keeps the focus).
        if (list.Count == Packs.Count && list.Select((p, i) => Packs[i].Name == p.Name && Packs[i].On == p.On).All(b => b))
        {
            return;
        }

        quiet = true;
        try
        {
            Packs.Clear();
            foreach ((string name, bool on) in list)
            {
                Packs.Add(new TexturePackItem(this, name, on));
            }
        }
        finally
        {
            quiet = false;
        }

        OnPropertyChanged(nameof(HasPacks));
        OnPropertyChanged(nameof(Note));
    }

    internal void Ticked(TexturePackItem item)
    {
        if (quiet)
        {
            return;
        }

        Pad($"[packs] \"{item.Name}\" {(item.On ? "on" : "off")}");
        Save();
    }

    /// <summary>
    /// Moves the pack at <paramref name="index"/> one place up (-1) or down (+1); the new index, or
    /// the same at the list's end. Saved by <see cref="EndMove"/>.
    /// </summary>
    public int Move(int index, int step)
    {
        int to = index + step;
        if (Locked || index < 0 || index >= Packs.Count || to < 0 || to >= Packs.Count)
        {
            return index;
        }

        TexturePackItem a = Packs[index], b = Packs[to];
        quiet = true;
        try
        {
            (a.Name, b.Name) = (b.Name, a.Name);
            (a.On, b.On) = (b.On, a.On);
            (a.IsHeld, b.IsHeld) = (b.IsHeld, a.IsHeld);
        }
        finally
        {
            quiet = false;
        }

        return to;
    }

    /// <summary>The row at <paramref name="index"/> is taken to move (it stands out).</summary>
    public void BeginMove(int index)
    {
        if (index >= 0 && index < Packs.Count)
        {
            Packs[index].IsHeld = true;
        }
    }

    /// <summary>Let go: the row stops standing out; when it moved, the new order is saved.</summary>
    public void EndMove(bool moved)
    {
        foreach (TexturePackItem p in Packs)
        {
            p.IsHeld = false;
        }

        if (moved)
        {
            Pad($"[packs] moved: {string.Join(" | ", Packs.Select(p => (p.On ? "+" : "-") + p.Name))}");
            Save();
        }
    }

    private void Save()
    {
        try
        {
            SettingsFile.WritePacks(SaveFolder.SettingsIni, schema(), [.. Packs.Select(p => (p.Name, p.On))]);
            ErrorSaid = null;
        }
        catch (InvalidOperationException)
        {
            ErrorSaid = S("The packs couldn't be saved: settings.ini isn't there yet. Start the game once, then choose them.");
        }
        catch (Exception e) when (e is IOException or UnauthorizedAccessException)
        {
            ErrorSaid = S("settings.ini couldn't be saved: {0}", e.Message);
        }
    }

    /// <summary>The test log (CONKER_LAUNCHER_PAD_LOG), when one is set.</summary>
    public Action<string>? Log { get; set; }

    private void Pad(string line) => Log?.Invoke(line);
}
