//header guard
#ifndef HEADER_ENVUTIL
#define HEADER_ENVUTIL

//imports
#include <stdbool.h>

//errors
typedef enum {
  ENV_LOAD_OK,
  ENV_LOAD_ERR_NO_FILE,
  ENV_LOAD_ERR_LINE_EXCEED_CAPAC,
  ENV_LOAD_ERR_SETENV_ERR,
  ENV_LOAD_ERR_PARSE_ERR,
  ENV_LOAD_ERR_FILE_READ_ERR,
} env_load_error;

typedef struct {
  env_load_error errType;
  char **loadedKeys;
  size_t loadedKeysCount;
} env_load_data_t;

typedef enum {
  ENV_LOOK_OK_FILE,
  ENV_LOOK_OK_ENV,
  ENV_LOOK_ERR_NO_FILE,
  ENV_LOOK_ERR_LINE_EXCEED_CAPAC,
  ENV_LOOK_ERR_PARSE_ERR,
  ENV_LOOK_ERR_FILE_READ_ERR,
  ENV_LOOK_ERR_KEY_NOT_FOUND,
  ENV_LOOK_ERR_VALUE_EXCEED_CAPAC,
  ENV_LOOK_ERR_MATCH_KEY_NULL,
  ENV_LOOK_ERR_OUT_VALUE_NULL,
  ENV_LOOK_ERR_OUT_CAPAC_ZERO,
} env_lookup_error;

env_load_error env_load(const char *restrict filePath, const bool overwrite);
env_load_data_t env_load_get_data(const char *restrict filePath, const bool overwrite);
void env_free_load_data(env_load_data_t *restrict data);
env_lookup_error env_lookup(const char *restrict filePath, const char *restrict matchKey, char *restrict outValue, const size_t outValueSize);

int _load_line_into_buff(char *restrict buffer, size_t *restrict pCount, FILE *restrict file);
int _load_chunk_into_buff(char *restrict buffer, size_t *restrict pCount, FILE *restrict file);
int _get_kvp_from_buff(char *restrict buffer, size_t count, char *restrict key, char *restrict value);
int _get_kvp_from_buff_intrpt(char *restrict buffer, size_t count, const char *restrict cmpKey, char *restrict value, const size_t valueSize);
int _parse_value(char *restrict buffer, size_t count, char *restrict value, const size_t valueSize, size_t i);

#endif

#ifdef ENVUTIL_IMPLEMENTATION

//constants
#ifndef ENV_UTIL_MAX_BUFF_SIZE
  #define ENV_UTIL_MAX_BUFF_SIZE 4096
#endif
#ifndef ENV_UTIL_BUFF_LOAD_SIZE
  #define ENV_UTIL_BUFF_LOAD_SIZE 64
#endif
#ifndef ENV_UTIL_KEY_BUFF_SIZE
  #define ENV_UTIL_KEY_BUFF_SIZE 1024
#endif
#ifndef ENV_UTIL_VALUE_BUFF_SIZE
  #define ENV_UTIL_VALUE_BUFF_SIZE 2048
#endif
#ifndef ENV_UTIL_RET_KEYS_DEFAULT_CAPAC
  #define ENV_UTIL_RET_KEYS_DEFAULT_CAPAC 8
#endif

//imports
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#endif

#ifdef __unix__
#define _env_set(key, value, overwrite) setenv(key, value, overwrite)
#elif defined(_WIN32)
#define _env_set(key, value, overwrite) w_env_set(key, value, overwrite)
int w_env_set(const char *restrict key, const char *restrict value, const bool overwrite){
  char tempBuff[2];
  if(GetEnvironmenA_IMPLEMENTATIONtVariable(key, tempBuff, sizeof(tempBuff)) != 0 && !overwrite) return 0;
  return SetEnvironmentVariable(key, value) == 0 ? 1 : 0;
}
#endif

/*
parses a file and loads all of it's entries into the current environment (the loaded values are accessible
with getenv())
if <overwrite> is true, it will overwrite the values of already present envvars with ones found in the file, otherwise it will skip them
if <filePath> is NULL, it will default to the file .env in the current directory
*/
env_load_error env_load(const char *restrict filePath, const bool overwrite){
  FILE *file = fopen(filePath != NULL ? filePath : "./.env", "r");
  if(file == NULL) return ENV_LOAD_ERR_NO_FILE;

  char buffer[ENV_UTIL_MAX_BUFF_SIZE] = {0};
  size_t bufferCount = 0;//count does not include the null term
  int result;
  bool returnNextLoop = false;
  for(;;){
    result = _load_line_into_buff(buffer, &bufferCount, file);
    if(result != 0 && result != -1) break;
    if(result == -1) returnNextLoop = true;
    char key[ENV_UTIL_KEY_BUFF_SIZE], value[ENV_UTIL_VALUE_BUFF_SIZE];
    result = _get_kvp_from_buff(buffer, bufferCount, key, value);
    if(result == 0){
      if(_env_set(key, value, overwrite) == 0){
        if(returnNextLoop) break;
        else continue;
      }
      fclose(file);
      return ENV_LOAD_ERR_SETENV_ERR;
    }
    if(returnNextLoop) break;
    if(result == -1 || result == -2) continue;
    fclose(file);
    return ENV_LOAD_ERR_PARSE_ERR;
  }
  fclose(file);
  if(result == 3) return ENV_LOAD_ERR_LINE_EXCEED_CAPAC;
  if(result == 2) return ENV_LOAD_ERR_FILE_READ_ERR;
  return ENV_LOAD_OK;
}

#define _env_push_arr(arr, capac, count, item) do {\
  if(capac == count){\
    capac *= 2;\
    arr = realloc(arr, sizeof(item) * capac);\
  }\
  arr[count++] = item;\
} while(false);

/*
!THIS FUNCTION ALLOCATES MEMORY!
parses a file and loads all of it's entries into the current environment (the loaded values are accessible
with getenv()), and also returns data about the set keys (if <overwrite> is false, and a key is found in the file,
but the key already is present in the environment, then it WON'T be present in the output)
if <overwrite> is true, it will overwrite the values of already present envvars with ones found in the file, otherwise it will skip them
if <filePath> is NULL, it will default to the file .env in the current directory
the returned data contains the error type, an array containing strings of the set keys, and the count of set keys
the returned data is heap-allocated, so it needs to be free (see env_free_load_data)
*/
env_load_data_t env_load_get_data(const char *restrict filePath, const bool overwrite){
  FILE *file = fopen(filePath != NULL ? filePath : "./.env", "r");
  if(file == NULL) return (env_load_data_t){ENV_LOAD_ERR_NO_FILE};

  size_t loadedKeysCapacity = ENV_UTIL_RET_KEYS_DEFAULT_CAPAC;
  char **loadedKeys = malloc(sizeof(char*) * loadedKeysCapacity);
  size_t loadedKeysCount = 0;

  char buffer[ENV_UTIL_MAX_BUFF_SIZE];
  size_t bufferCount = 0;//count does not include the null term
  int result;
  bool returnNextLoop = false;
  for(;;){
    result = _load_line_into_buff(buffer, &bufferCount, file);
    if(result != 0 && result != -1) break;
    if(result == -1) returnNextLoop = true;
    char key[ENV_UTIL_KEY_BUFF_SIZE], value[ENV_UTIL_VALUE_BUFF_SIZE];
    result = _get_kvp_from_buff(buffer, bufferCount, key, value);
    if(result == 0){
      bool pushKey = overwrite || getenv(key) == NULL;//push the key if either we overwrite the var, or the var doesn't exist in the env
      if(_env_set(key, value, overwrite) == 0){
        if(pushKey){
          char *keyCopy = malloc(strlen(key) + 1);
          strcpy(keyCopy, key);
          _env_push_arr(loadedKeys, loadedKeysCapacity, loadedKeysCount, keyCopy);
        }
        if(returnNextLoop) break;
        continue;
      }
      fclose(file);
      return (env_load_data_t){ENV_LOAD_ERR_SETENV_ERR};
    }
    if(returnNextLoop) break;
    if(result == -1 || result == -2) continue;
    fclose(file);
    return (env_load_data_t){ENV_LOAD_ERR_PARSE_ERR};
  }
  fclose(file);
  if(result == 3) return (env_load_data_t){ENV_LOAD_ERR_LINE_EXCEED_CAPAC};
  if(result == 2) return (env_load_data_t){ENV_LOAD_ERR_FILE_READ_ERR};
  return (env_load_data_t){ENV_LOAD_OK, loadedKeys, loadedKeysCount};
}

#undef _env_push_arr
#undef _env_set

/*
frees each element of <data->loadedKeys> and <data->loadedKeys> itself, and also sets <data->loadedKeys> to NULL
*/
void env_free_load_data(env_load_data_t *restrict data){
  for(size_t i = 0;i < data->loadedKeysCount;i++) free(data->loadedKeys[i]);
  free(data->loadedKeys);
  data->loadedKeys = NULL;
}

/*
searches through a file and tries to find a key that matches <key>, and puts the corresponding value into <value>
if the entry is not found in the file, it looks through the environment
if <filePath> is NULL, it will default to the file .env in the current directory
*/
env_lookup_error env_lookup(const char *restrict filePath, const char *restrict matchKey, char *restrict outValue, const size_t outValueSize){
  if(matchKey == NULL) return ENV_LOOK_ERR_MATCH_KEY_NULL;
  if(outValue == NULL) return ENV_LOOK_ERR_OUT_VALUE_NULL;
  if(outValueSize == 0) return ENV_LOOK_ERR_OUT_CAPAC_ZERO;
  FILE *file = fopen(filePath != NULL ? filePath : "./.env", "r");
  if(file == NULL){
    char *value = getenv(matchKey);
    if(value == NULL) return ENV_LOOK_ERR_KEY_NOT_FOUND;
    if(strlen(value) + 1 > outValueSize) return ENV_LOOK_ERR_VALUE_EXCEED_CAPAC;
    strcpy(outValue, value);
    return ENV_LOOK_OK_ENV;
  }

  char buffer[ENV_UTIL_MAX_BUFF_SIZE];
  size_t bufferCount = 0;//count does not include the null term
  int result;
  bool returnNextLoop = false;
  for(;;){
    result = _load_line_into_buff(buffer, &bufferCount, file);
    if(result != 0 && result != -1) break;
    if(result == -1) returnNextLoop = true;
    result = _get_kvp_from_buff_intrpt(buffer, bufferCount, matchKey, outValue, outValueSize);
    switch(result){
      case 0:
        fclose(file);
        return ENV_LOOK_OK_FILE;
      case 10:
        if(returnNextLoop) break;
        continue;
      case 6:
        fclose(file);
        return ENV_LOOK_ERR_VALUE_EXCEED_CAPAC;
      default:
        fclose(file);
        return ENV_LOOK_ERR_PARSE_ERR;
    }
    if(returnNextLoop) break;
  }
  fclose(file);
  if(result == 3) return ENV_LOOK_ERR_LINE_EXCEED_CAPAC;
  if(result == 2) return ENV_LOOK_ERR_FILE_READ_ERR;

  //env not found in file
  char *value = getenv(matchKey);
  if(value == NULL) return ENV_LOOK_ERR_KEY_NOT_FOUND;
  if(strlen(value) + 1 > outValueSize) return ENV_LOOK_ERR_VALUE_EXCEED_CAPAC;
  strcpy(outValue, value);
  return ENV_LOOK_OK_ENV;
}

//Loads characters into <*pBuffer> until it finds a '\n', and increases <*pCount>
//returns 0 if successfull, 1 if eof, 2 if file read error, 3 if <*pBuffer> doesn't have capacity for load, -1 if eof but found data
int _load_line_into_buff(char *restrict buffer, size_t *restrict pCount, FILE *restrict file){
  *pCount = 0;
  for(size_t i = 0;i < ENV_UTIL_MAX_BUFF_SIZE;i++) buffer[i] = '\0';
  for(;;){
    int result = _load_chunk_into_buff(buffer, pCount, file);
    if(result != 0) return result;
    for(;;(*pCount)--){
      if(buffer[(*pCount)] == '\0'){
        if(*pCount == 0) return 0;
        continue;
      }
      else if(buffer[(*pCount)] == '\n'){
        (*pCount)++;
        return 0;
      }
      else break;
    }
  }
}

//Loads <ENV_LOAD_BUFF_LOAD_SIZE> characters into the available space in <*pBuffer>, and increases <*pCount>
//returns 0 if successfull, 1 if eof, 2 if file read error, and 3 if <*pBuffer> doesn't have capacity for load
int _load_chunk_into_buff(char *restrict buffer, size_t *restrict pCount, FILE *restrict file){
  if(*pCount + ENV_UTIL_BUFF_LOAD_SIZE - 1 > ENV_UTIL_MAX_BUFF_SIZE) return 2;
  char *output = fgets(buffer + *pCount, ENV_UTIL_BUFF_LOAD_SIZE, file);
  *pCount += ENV_UTIL_BUFF_LOAD_SIZE - 1;
  return output == NULL ? (ferror(file) ? 2 : 1) : 0;
}

/*
Iterates through <buffer> and attempts to load the key into <*pKey> and the value into <*pValue> using '=' as a separator
returns a 0 if succesfull, 1 if unexpected '\n', 2 if key is empty, 3 if key is too long, 4 if equals is not present before value, 5 if value is empty, 6 if value is too long, 7 if quote is not closed, 8 if bad escape seq,
9 if invalid char, -1 if line is a comment, -2 if line is empty
buffer should be '\n' terminated
*/

int _get_kvp_from_buff(char *restrict buffer, size_t count, char *restrict key, char *restrict value){
  size_t i = 0;
  size_t copyStartIndex = 0;

  for(;;i++){//go until non-space char
    if(buffer[i] == '#') return -1;
    if(buffer[i] == '\0') return 9;
    if(buffer[i] == '\n') return -2;
    if(buffer[i] != ' ') break;
  }

  copyStartIndex = i;

  for(;;i++){//go until space or equals
    if(buffer[i] == '\0' || buffer[i] == '#') return 9;
    if(buffer[i] == '\n') return 1;
    if(buffer[i] == ' ' || buffer[i] == '=') break;
  }
  size_t copyCount = i - copyStartIndex;

  if(copyCount == 0) return 2;
  if(copyCount > ENV_UTIL_KEY_BUFF_SIZE - 1) return 3;

  memcpy(key, buffer + copyStartIndex, copyCount);
  key[copyCount] = '\0';
  
  if(buffer[i] != '='){
    for(;;i++){//go until non-space char
      if(buffer[i] == '\0' || buffer[i] == '#') return 9;
      if(buffer[i] == '\n') return 1;
      if(buffer[i] != ' ') break;
    }
    if(buffer[i] != '=') return 4;
  }

  return _parse_value(buffer, count, value, ENV_UTIL_VALUE_BUFF_SIZE, i);
}

/*
Iterates through <buffer> and attempts to match the key to <matchKey>, if match fails returns prematureley if key doesnt match
returns a 0 if succesfull, 1 if unexpected '\n', 2 if key is empty, 3 if key is too long, 4 if equals is not present before value, 5 if value is empty, 6 if value is too long, 7 if quote is not closed, 8 if bad escape seq,
9 if invalid char, 10 if key doesn't match, -1 if line is a comment, -2 if line is empty
buffer should be '\n' terminated
*/
int _get_kvp_from_buff_intrpt(char *restrict buffer, size_t count, const char *restrict cmpKey, char *restrict value, const size_t valueSize){
  size_t i = 0;
  size_t copyStartIndex = 0;
  size_t copyCount;

  for(;;i++){//go until n
    if(buffer[i] == '#') return -1;
    if(buffer[i] == '\0') return 9;
    if(buffer[i] == '\n') return -2;
    if(buffer[i] != ' ') break;
  }

  copyStartIndex = i;
  while(cmpKey[i - copyStartIndex] != '\0'){
    if(buffer[i] == '\0' || buffer[i] == '#') return 9;
    if(buffer[i] == '\n') return 1;
    if(cmpKey[i - copyStartIndex] != buffer[i]) return 10;
    i++;
  }
  
  if(buffer[i] != '='){
    for(;;i++){//go until non-space char
      if(buffer[i] == '\0' || buffer[i] == '#') return 9;
      if(buffer[i] == '\n') return 1;
      if(buffer[i] != ' ') break;
    }
    if(buffer[i] != '=') return 4;
  }

  return _parse_value(buffer, count, value, valueSize, i);
}

//sub function used in _get_kvp_from_buff and _get_kvp_from_buff_intrpt
//iterates through <buffer> and copies the value into <value>
//for error code meaning, go read the comments on those funtions
int _parse_value(char *restrict buffer, size_t count, char *restrict value, const size_t valueSize, size_t i){
  for(i++;;i++){//go until non-space char
    if(buffer[i] == '\0' || buffer[i] == '#') return 9;
    if(buffer[i] == '\n') return 5;//returns 5 because value is empty
    if(buffer[i] != ' ') break;
  }

  const bool isInQuote = buffer[i] == '\"';

  size_t valueCount = 0;

  if(isInQuote) i++;
  for(;valueCount <= valueSize - 1;i++){
    switch(buffer[i]){
      case '\0':
        return 9;
      case '\n':
        if(isInQuote) return 7;
        value[valueCount] = '\0';
        return 0;
      case ' ':
      case '#':
        if(!isInQuote){
          value[valueCount] = '\0';
          return 0;
        }
        value[valueCount++] = buffer[i];
        break;
      case '\"':
        if(!isInQuote) return 9;
        value[valueCount] = '\0';
        return 0;
      case '\'':
        return 9;
      case '\\':
        i++;
        switch(buffer[i]){
          case '\\':
          case '\"':
          case '\'':
            value[valueCount++] = buffer[i];
            break;
          case 'n':
            value[valueCount++] = '\n';
            break;
          case 'r':
            value[valueCount++] = '\r';
            break;
          default:
            return 8;
        }
        break;
      default:
        value[valueCount++] = buffer[i];
        break;
    }
  }
  return 6;
}
#endif
