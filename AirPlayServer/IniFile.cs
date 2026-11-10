using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace AirPlayServer;

// A minimal INI file in UTF-8, compatible with files written by the Windows
// profile API. Section and key names are case-insensitive, as they are there.
// Entries this app does not know about are kept when it saves.
internal sealed class IniFile
{
    private readonly string path;
    private readonly List<Section> sections = [];

    private sealed class Section(string name)
    {
        public string Name { get; } = name;
        public List<KeyValuePair<string, string>> Entries { get; } = [];
    }

    private IniFile(string path)
    {
        this.path = path;
    }

    public static IniFile Load(string path)
    {
        IniFile file = new(path);
        if (!File.Exists(path))
        {
            return file;
        }

        Section? current = null;
        foreach (string rawLine in File.ReadAllLines(path, Encoding.UTF8))
        {
            string line = rawLine.Trim();
            if (line.Length == 0 || line[0] == ';')
            {
                continue;
            }
            if (line[0] == '[' && line[^1] == ']')
            {
                current = file.GetSection(line[1..^1].Trim(), create: true);
                continue;
            }
            int equals = line.IndexOf('=');
            if (current != null && equals > 0)
            {
                current.Entries.Add(new(line[..equals].Trim(), line[(equals + 1)..].Trim()));
            }
        }
        return file;
    }

    public string? Get(string section, string key)
    {
        Section? found = GetSection(section, create: false);
        if (found == null)
        {
            return null;
        }
        int index = found.Entries.FindIndex(e => string.Equals(e.Key, key, StringComparison.OrdinalIgnoreCase));
        return index >= 0 ? found.Entries[index].Value : null;
    }

    // A null value removes the key.
    public void Set(string section, string key, string? value)
    {
        Section? found = GetSection(section, create: value != null);
        if (found == null)
        {
            return;
        }
        int index = found.Entries.FindIndex(e => string.Equals(e.Key, key, StringComparison.OrdinalIgnoreCase));
        if (value == null)
        {
            if (index >= 0)
            {
                found.Entries.RemoveAt(index);
            }
        }
        else if (index >= 0)
        {
            found.Entries[index] = new(found.Entries[index].Key, value);
        }
        else
        {
            found.Entries.Add(new(key, value));
        }
    }

    // Writes to a temporary file first so a crash cannot leave a truncated file.
    public void Save()
    {
        StringBuilder text = new();
        foreach (Section section in sections)
        {
            text.Append('[').Append(section.Name).Append("]\r\n");
            foreach (KeyValuePair<string, string> entry in section.Entries)
            {
                text.Append(entry.Key).Append('=').Append(entry.Value).Append("\r\n");
            }
        }

        string directory = Path.GetDirectoryName(path)!;
        Directory.CreateDirectory(directory);
        string temporary = path + ".tmp";
        File.WriteAllText(temporary, text.ToString(), new UTF8Encoding(false));
        File.Move(temporary, path, overwrite: true);
    }

    private Section? GetSection(string name, bool create)
    {
        foreach (Section section in sections)
        {
            if (string.Equals(section.Name, name, StringComparison.OrdinalIgnoreCase))
            {
                return section;
            }
        }
        if (!create)
        {
            return null;
        }
        Section added = new(name);
        sections.Add(added);
        return added;
    }
}
