#include <time.h>
#include "qidl.h"

void visit_type_tm(Visitor *v, struct tm *obj, const char *name, Error **errp)
{
    visit_start_struct(v, NULL, "struct tm", name, 0, errp);
    visit_type_int32(v, &obj->tm_year, "tm_year", errp);
    visit_type_int32(v, &obj->tm_mon, "tm_mon", errp);
    visit_type_int32(v, &obj->tm_mday, "tm_mday", errp);
    visit_type_int32(v, &obj->tm_hour, "tm_hour", errp);
    visit_type_int32(v, &obj->tm_min, "tm_min", errp);
    visit_type_int32(v, &obj->tm_sec, "tm_sec", errp);
    visit_end_struct(v, errp);
}
