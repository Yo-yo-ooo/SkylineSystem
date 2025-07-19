#include <arch/x86_64/allin.h>

MStack::MStack(const char* name, const char* filename, int32_t line, int32_t layer, uint64_t time, bool close)
{
    this->name = name;
    this->filename = filename;
    this->line = line;
    this->layer = layer;
    this->time = time;
    this->close = close;
}

MStack::MStack(const char* name, const char* filename, int32_t line)
{
    this->name = name;
    this->filename = filename;
    this->line = line;
}

MStack::MStack(const char* name, const char* filename)
{
    this->name = name;
    this->filename = filename;
    line = 0;
}

MStack::MStack()
{
    name = "<NONE>";
    filename = "<NONE>";
    line = -1;

    layer = 0;
    time = 0;
    close = false;
}

static bool StrEquals(const char* a, const char* b)
{
    int32_t diff = strlen(a) - strlen(b);
    if (diff != 0)
        return false;

    int32_t index = 0;
    while (!(a[index] == 0 && b[index] == 0))
    {
        if (a[index] != b[index])
            return false;
        index++;
    }

    return true;
}

bool MStack::operator==(MStack other)
{
    return StrEquals(name, other.name) && StrEquals(filename, other.filename) && (line == other.line)
    && (time == other.time) && (layer == other.layer) && (close == other.close);
}

