#include "global.h"

bool file_read_string(const char *path, char *out, size_t out_size)
{
    FILE *fp;
    size_t len;

    if (path == NULL || out == NULL || out_size == 0U)
        return false;

    fp = fopen(path, "r");
    if (fp == NULL)
        return false;

    memset(out, 0, out_size);

    if (fgets(out, (int)out_size, fp) == NULL)
    {
        fclose(fp);
        return false;
    }

    fclose(fp);

    len = strlen(out);

    while (len > 0U && (out[len - 1U] == '\n' || out[len - 1U] == '\r'))
    {
        out[len - 1U] = '\0';
        len--;
    }

    return true;
}


bool file_write_string(const char *path, const char *data)
{
    FILE *fp;
    size_t data_len;
    size_t written;

    if (path == NULL || data == NULL)
        return false;

    fp = fopen(path, "w");
    if (fp == NULL)
        return false;

    data_len = strlen(data);
    written  = fwrite(data, 1U, data_len, fp);

    if (fflush(fp) != 0)
    {
        fclose(fp);
        return false;
    }

    fclose(fp);

    return (written == data_len);
}
