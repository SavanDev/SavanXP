# cmake -P helper for cmake/NetSurf.cmake: what libhubbub's Makefile does with
# sed on gperf's output, making the element table static.
file(READ "${INPUT}" text)
string(REGEX REPLACE "(^|\n)const struct element_type_map" "\\1static const struct element_type_map" text "${text}")
file(WRITE "${OUTPUT}" "${text}")
