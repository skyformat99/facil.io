/*
Copyright: Boaz Segev, 2018-2019
License: MIT
*/
#define INCLUDE_MUSTACHE_IMPLEMENTATION 1
#include <fiobj_mustache.h>

#ifndef FIO_IGNORE_MACRO
/**
 * This is used internally to ignore macros that shadow functions (avoiding
 * named arguments when required).
 */
#define FIO_IGNORE_MACRO
#endif

/**
 * Loads a mustache template, converting it into an opaque instruction array.
 *
 * Returns a pointer to the instruction array.
 *
 * The `folder` argument should contain the template's root folder which would
 * also be used to search for any required partial templates.
 *
 * The `filename` argument should contain the template's file name.
 */
mustache_s *fiobj_mustache_load(fio_str_info_s filename) {
  return mustache_load(.filename = filename.buf, .filename_len = filename.len);
}

/**
 * Loads a mustache template, converting it into an opaque instruction array.
 *
 * Returns a pointer to the instruction array.
 *
 * The `folder` argument should contain the template's root folder which would
 * also be used to search for any required partial templates.
 *
 * The `filename` argument should contain the template's file name.
 */
mustache_s *fiobj_mustache_new FIO_IGNORE_MACRO(mustache_load_args_s args) {
  return mustache_load FIO_IGNORE_MACRO(args);
}

/** Free the mustache template */
void fiobj_mustache_free(mustache_s *mustache) { mustache_free(mustache); }

/**
 * Renders a template into an existing FIOBJ String (`dest`'s end), using the
 * information in the `data` object.
 *
 * Returns FIOBJ_INVALID if an error occured and a FIOBJ String on success.
 */
FIOBJ fiobj_mustache_build2(FIOBJ dest, const mustache_s *mustache,
                            FIOBJ data) {
  mustache_build(mustache, .udata1 = (void *)dest, .udata2 = (void *)data);
  return dest;
}

/**
 * Creates a FIOBJ String containing the rendered template using the information
 * in the `data` object.
 *
 * Returns FIOBJ_INVALID if an error occured and a FIOBJ String on success.
 */
FIOBJ fiobj_mustache_build(const mustache_s *mustache, FIOBJ data) {
  if (!mustache)
    return FIOBJ_INVALID;
  return fiobj_mustache_build2(
      fiobj_str_new_buf(mustache->u.read_only.data_length), mustache, data);
}

/* *****************************************************************************
Mustache Callbacks
***************************************************************************** */

static inline FIOBJ fiobj_mustache_find_obj_absolute(FIOBJ parent, FIOBJ key) {
  if (!FIOBJ_TYPE_IS(parent, FIOBJ_T_HASH))
    return FIOBJ_INVALID;
  FIOBJ o = fiobj_hash_get2(parent, key);
  return o;
}

static inline FIOBJ fiobj_mustache_find_obj_tree(mustache_section_s *section,
                                                 const char *name,
                                                 uint32_t name_len) {
  FIOBJ_STR_TEMP_VAR(key)
  fiobj_str_write(key, name, name_len);
  do {
    FIOBJ tmp = fiobj_mustache_find_obj_absolute((FIOBJ)section->udata2, key);
    if (tmp != FIOBJ_INVALID) {
      return tmp;
    }
  } while ((section = mustache_section_parent(section)));
  FIOBJ_STR_TEMP_DESTROY(key);
  return FIOBJ_INVALID;
}

static inline FIOBJ fiobj_mustache_find_obj(mustache_section_s *section,
                                            const char *name,
                                            uint32_t name_len) {
  FIOBJ tmp = fiobj_mustache_find_obj_tree(section, name, name_len);
  if (tmp != FIOBJ_INVALID)
    return tmp;
  /* interpolate sections... */
  uint32_t dot = 0;
  while (dot < name_len && name[dot] != '.')
    ++dot;
  if (dot == name_len)
    return FIOBJ_INVALID;
  tmp = fiobj_mustache_find_obj_tree(section, name, dot);
  if (!tmp) {
    return FIOBJ_INVALID;
  }
  ++dot;
  for (;;) {
    FIOBJ_STR_TEMP_VAR(key);
    fiobj_str_write(key, name + dot, name_len - dot);
    FIOBJ obj = fiobj_mustache_find_obj_absolute(tmp, key);
    fiobj_str_destroy(key);
    if (obj != FIOBJ_INVALID)
      return obj;
    name += dot;
    name_len -= dot;
    dot = 0;
    while (dot < name_len && name[dot] != '.')
      ++dot;
    if (dot == name_len) {
      return FIOBJ_INVALID;
    }
    fiobj_str_write(key, name, dot);
    tmp = fiobj_mustache_find_obj_absolute(tmp, key);
    FIOBJ_STR_TEMP_DESTROY(key);
    if (tmp == FIOBJ_INVALID)
      return FIOBJ_INVALID;
    ++dot;
  }
}

/**
 * Called when an argument name was detected in the current section.
 *
 * A conforming implementation will search for the named argument both in the
 * existing section and all of it's parents (walking backwards towards the root)
 * until a value is detected.
 *
 * A missing value should be treated the same as an empty string.
 *
 * A conforming implementation will output the named argument's value (either
 * HTML escaped or not, depending on the `escape` flag) as a string.
 */
static int mustache_on_arg(mustache_section_s *section, const char *name,
                           uint32_t name_len, unsigned char escape) {
  FIOBJ o = fiobj_mustache_find_obj(section, name, name_len);
  if (!o)
    return 0;
  fio_str_info_s i = fiobj2cstr(o);
  if (!i.len)
    return 0;
  return mustache_write_text(section, i.buf, i.len, escape);
}

/**
 * Called when simple template text (string) is detected.
 *
 * A conforming implementation will output data as a string (no escaping).
 */
static int mustache_on_text(mustache_section_s *section, const char *data,
                            uint32_t data_len) {
  FIOBJ dest = (FIOBJ)section->udata1;
  fiobj_str_write(dest, data, data_len);
  return 0;
}

/**
 * Called for nested sections, must return the number of objects in the new
 * subsection (depending on the argument's name).
 *
 * Arrays should return the number of objects in the array.
 *
 * `true` values should return 1.
 *
 * `false` values should return 0.
 *
 * A return value of -1 will stop processing with an error.
 *
 * Please note, this will handle both normal and inverted sections.
 */
static int32_t mustache_on_section_test(mustache_section_s *section,
                                        const char *name, uint32_t name_len,
                                        uint8_t callable) {
  FIOBJ o = fiobj_mustache_find_obj(section, name, name_len);
  if (!o || FIOBJ_TYPE_IS(o, FIOBJ_T_FALSE))
    return 0;
  if (FIOBJ_TYPE_IS(o, FIOBJ_T_ARRAY))
    return fiobj_array_count(o);
  return 1;
  (void)callable; /* FIOBJ doesn't support lambdas */
}

/**
 * Called when entering a nested section.
 *
 * `index` is a zero based index indicating the number of repetitions that
 * occurred so far (same as the array index for arrays).
 *
 * A return value of -1 will stop processing with an error.
 *
 * Note: this is a good time to update the subsection's `udata` with the value
 * of the array index. The `udata` will always contain the value or the parent's
 * `udata`.
 */
static int mustache_on_section_start(mustache_section_s *section,
                                     char const *name, uint32_t name_len,
                                     uint32_t index) {
  FIOBJ o = fiobj_mustache_find_obj(section, name, name_len);
  if (!o)
    return -1;
  if (FIOBJ_TYPE_IS(o, FIOBJ_T_ARRAY))
    section->udata2 = (void *)fiobj_array_get(o, index);
  else
    section->udata2 = (void *)o;
  return 0;
}

/**
 * Called for cleanup in case of error.
 */
static void mustache_on_formatting_error(void *udata1, void *udata2) {
  (void)udata1;
  (void)udata2;
}

/* *****************************************************************************
Testing
***************************************************************************** */

#if TEST || DEBUG
static inline void mustache_save2file(char const *filename, char const *data,
                                      size_t length) {
  int fd = open(filename, O_CREAT | O_RDWR, 0);
  if (fd == -1) {
    perror("Couldn't open / create file for template testing");
    exit(-1);
  }
  fchmod(fd, 0777);
  if (pwrite(fd, data, length, 0) != (ssize_t)length) {
    perror("Mustache template write error");
    exit(-1);
  }
  close(fd);
}

void fiobj_mustache_test(void) {
  char const *template =
      "{{=<< >>=}}* Users:\r\n<<#users>><<id>>. <<& name>> "
      "(<<name>>)\r\n<</users>>\r\nNested: <<& nested.item >>.";
  char const *template_name = "mustache_test_template.mustache";
  fprintf(stderr, "===============\n");
  fprintf(stderr, "* Testing FIOBJ mustache extension.\n");
  mustache_save2file(template_name, template, strlen(template));
  mustache_s *m =
      fiobj_mustache_load((fio_str_info_s){.buf = (char *)template_name});
  unlink(template_name);
  FIO_ASSERT(m, "fiobj_mustache_load failed.\n");
  FIOBJ data = fiobj_hash_new();
  FIOBJ tmp = fiobj_str_new();
  fiobj_str_write(tmp, "users", 5);
  FIOBJ ary = fiobj_array_new();
  fiobj_hash_set2(data, tmp, ary);
  fiobj_free(tmp);
  for (uint8_t i = 0; i < 4; ++i) {
    FIOBJ id = fiobj_str_new();
    fiobj_str_write_i(id, i);
    FIOBJ name = fiobj_str_new_cstr("User ", 5);
    fiobj_str_write_i(name, i);
    FIOBJ usr = fiobj_hash_new();
    tmp = fiobj_str_new_cstr("id", 2);
    fiobj_hash_set2(usr, tmp, id);
    fiobj_free(tmp);
    tmp = fiobj_str_new_cstr("name", 4);
    fiobj_hash_set2(usr, tmp, name);
    fiobj_free(tmp);
    fiobj_array_push(ary, usr);
  }
  tmp = fiobj_str_new_cstr("nested", 6);
  ary = fiobj_hash_new();
  fiobj_hash_set2(data, tmp, ary);
  fiobj_free(tmp);
  tmp = fiobj_str_new_cstr("item", 4);
  fiobj_hash_set2(ary, tmp, fiobj_str_new_cstr("dot notation success", 20));
  fiobj_free(tmp);
  tmp = fiobj_mustache_build(m, data);
  fiobj_free(data);
  FIO_ASSERT(tmp, "fiobj_mustache_build failed!\n");
  if (0) {
    fprintf(stderr, "* Manual review:\n%s\n", fiobj2cstr(tmp).buf);
  }
  FIO_ASSERT(fiobj2cstr(tmp).len == 135,
             "FIOBJ mustache rendering error (length) %u != %u\n%s",
             fiobj2cstr(tmp).len, 135, fiobj2cstr(tmp).buf);
  FIO_ASSERT(!memcmp(fiobj2cstr(tmp).buf,
                     "* Users:\r\n"
                     "0. User 0 (User&#32;0)\r\n"
                     "1. User 1 (User&#32;1)\r\n"
                     "2. User 2 (User&#32;2)\r\n"
                     "3. User 3 (User&#32;3)\r\n"
                     "Nested: dot notation success.",
                     135),
             "FIOBJ mustache rendering error (content)");
  fiobj_free(tmp);
  fiobj_mustache_free(m);
}

#endif
