#ifdef linux
#define _XOPEN_SOURCE 500
#define _POSIX_C_SOURCE 200809L
#endif
#include <dirent.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <sys/time.h>
#include <iconv.h>
#include <unistd.h>
#include <limits.h>
#include <signal.h>
#include <sys/stat.h>
#include <ctype.h>
#include <errno.h>

#ifndef _WIN32
#include <ftw.h>
#include <pwd.h>
#include <netdb.h>
#include <arpa/inet.h>
#else
// Windows/MinGW compatibility
typedef int socklen_t;
#endif

#include "main.h"
#include "common.h"
#include "argv.h"

static const char* errors[] = {
  [_ERROR_SUCCESS] = "Unknown error",
  [ERROR_FATAL_ERROR] = "fatal error",
  [ERROR_FATAL_RECV_FAILED] = "recv failed",
  [ERROR_FATAL_SEND_FAILED] = "send failed",
  [ERROR_FATAL_FAILED_TO_CREATE_OS_RESOURCE] = "failed to create os resource",
  [ERROR_FATAL_CREATE_FILE_FAILED] = "create file failed",
  [ERROR_FATAL_FILE_WRITE_FAILED] = "file write failed",
  [ERROR_FILE_READ_FAILED] = "file read failed",
  [ERROR_SET_DATESTAMP_FAILED] = "set datestamp failed",
  [ERROR_SET_PROTECTION_FAILED] = "set protection failed",
  [ERROR_CD_FAILED] = "cd failed",
  [ERROR_EXEC_FAILED] = "exec failed",
  [ERROR_SUCK_ON_DIR] = "suck on dir",
};

const char*
util_getHistoryFile(void)
{
  static char buffer[PATH_MAX];
  snprintf(buffer, sizeof(buffer), "%s/.squirt_history", util_getHomeDir());
  return buffer;
}

const char*
util_getHomeDir(void)
{
#ifndef _WIN32
  struct passwd *pw = getpwuid(getuid());
  return  pw->pw_dir;
#else
  return getenv("USERPROFILE");
#endif
}

static int
util_getSockAddr(const char * host, int port, struct sockaddr_in * addr)
{
  struct hostent * remote;

  if ((remote = gethostbyname(host)) != NULL) {
    char **ip_addr;
    memcpy(&ip_addr, &(remote->h_addr_list[0]), sizeof(void *));
    memcpy(&addr->sin_addr.s_addr, ip_addr, sizeof(struct in_addr));
  } else if ((addr->sin_addr.s_addr = inet_addr(host)) == (unsigned long)INADDR_NONE) {
    return 0;
  }

  addr->sin_port = htons(port);
  addr->sin_family = AF_INET;

  return 1;
}


void
util_connect(const char* hostname)
{
  struct sockaddr_in sockAddr;
  int result;
  fd_set writefds;
  struct timeval timeout;
  socklen_t len;
  int error;
#ifndef _WIN32
  int flags;
#endif

  int port=NETWORK_PORT;

  char *colon=strstr(hostname,":");
  if(colon)
  {
          port=strtol(colon+1,NULL,10);
          *colon=0;   // end hostname string here
  }


  if (!util_getSockAddr(hostname, port, &sockAddr)) {
    goto error;
  }

  if ((main_socketFd = socket(AF_INET, SOCK_STREAM, 0)) < 0) {
    goto error;
  }

  // Set socket to non-blocking mode
#ifdef _WIN32
  u_long mode = 1;
  if (ioctlsocket(main_socketFd, FIONBIO, &mode) != 0) {
    goto error;
  }
#else
  flags = fcntl(main_socketFd, F_GETFL, 0);
  if (flags < 0) {
    goto error;
  }
  if (fcntl(main_socketFd, F_SETFL, flags | O_NONBLOCK) < 0) {
    goto error;
  }
#endif

  // Attempt to connect
  result = connect(main_socketFd, (struct sockaddr *)&sockAddr, sizeof(struct sockaddr_in));
  
  if (result < 0) {
#ifdef _WIN32
    int wsaError = WSAGetLastError();
    if (wsaError != WSAEWOULDBLOCK) {
      goto error;
    }
#else
    if (errno != EINPROGRESS) {
      goto error;
    }
#endif
    
    // Connection is in progress, wait for it to complete with timeout
    FD_ZERO(&writefds);
    FD_SET(main_socketFd, &writefds);
    
    timeout.tv_sec = 5;  // 5 second timeout
    timeout.tv_usec = 0;
    
    result = select(main_socketFd + 1, NULL, &writefds, NULL, &timeout);
    
    if (result <= 0) {
      // Timeout or error
      goto error;
    }
    
    // Check if connection was successful
    len = sizeof(error);
#ifdef _WIN32
    if (getsockopt(main_socketFd, SOL_SOCKET, SO_ERROR, (char*)&error, &len) < 0) {
#else
    if (getsockopt(main_socketFd, SOL_SOCKET, SO_ERROR, &error, &len) < 0) {
#endif
      goto error;
    }
    
    if (error != 0) {
#ifdef _WIN32
      WSASetLastError(error);
#else
      errno = error;
#endif
      goto error;
    }
  }

  // Restore socket to blocking mode
#ifdef _WIN32
  mode = 0;
  if (ioctlsocket(main_socketFd, FIONBIO, &mode) != 0) {
    goto error;
  }
#else
  // Clear the O_NONBLOCK flag to ensure blocking mode
  if (fcntl(main_socketFd, F_SETFL, flags & ~O_NONBLOCK) < 0) {
    goto error;
  }
#endif

  // Note: Socket-level timeouts (SO_RCVTIMEO/SO_SNDTIMEO) can cause issues
  // Connection timeout is already handled above with select() during connect

  // Reset connection error flag for new connection
  util_resetConnectionErrorFlag();

  return;
 error:
  if (main_socketFd >= 0) {
    close(main_socketFd);
    main_socketFd = -1;
  }
  fatalError("failed to connect to server %s:%d", hostname,port);
}


int
util_mkpath(const char *dir)
{
  int error = 0;
  char tmp[PATH_MAX];
  char *p = NULL;
  size_t len;
  int makeLast = 0;

  snprintf(tmp, sizeof(tmp),"%s",dir);

  len = strlen(tmp);

#ifdef _WIN32
  // On Windows, check for backslash at end
  if (tmp[len - 1] == '\\') {
    makeLast = 1;
    tmp[len - 1] = 0;
  }
#else
  // On Unix, check for forward slash at end
  if (tmp[len - 1] == '/') {
    makeLast = 1;
    tmp[len - 1] = 0;
  }
#endif

  for (p = tmp + 1; *p; p++) {
#ifdef _WIN32
    // On Windows, look for backslashes
    if (*p == '\\') {
      *p = 0;
      util_mkdir(tmp, S_IRWXU);
      *p = '\\';
    }
#else
    // On Unix, look for forward slashes
    if (*p == '/') {
      *p = 0;
      util_mkdir(tmp, S_IRWXU);
      *p = '/';
    }
#endif
  }

  if (makeLast) {
    util_mkdir(tmp, S_IRWXU);
  }

  return error;
}


int
util_dirOperation(const char* directory, void (*operation)(const char* filename, void* data), void* data)
{
  DIR * dir =  opendir(directory);
  if (!dir) {
    return -1;
  }

  struct dirent *dp;
  while ((dp = readdir(dir)) != NULL) {
    if (operation) {
      operation(dp->d_name, data);
    }
  }

  closedir(dir);
  return 1;
}


#ifndef _WIN32
static int
_util_nftwRmFiles(const char *pathname, const struct stat *sbuf, int type, struct FTW *ftwb)
{
  (void)sbuf,(void)type,(void)ftwb;

  return remove(pathname);
}


int
util_rmdir(const char *dir)
{
  int error = 0;

  if (nftw(dir, _util_nftwRmFiles,10, FTW_DEPTH|FTW_MOUNT|FTW_PHYS) < 0) {
    error = -1;
  }

  return error;
}
#else
int
util_rmdir(const char* path)
{
  char* doubleTerminatedPath = malloc(strlen(path)+3);
  memset(doubleTerminatedPath, 0, strlen(path)+3);
  strcpy(doubleTerminatedPath, path);

  SHFILEOPSTRUCT fileOperation;
  fileOperation.wFunc = FO_DELETE;
  fileOperation.pFrom = doubleTerminatedPath;
  fileOperation.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION;

  return SHFileOperation(&fileOperation);

}
#endif


int
util_mkdir(const char *path, uint32_t mode)
{
#ifndef _WIN32
  struct stat st;
  if (stat(path, &st) == 0 && S_ISDIR(st.st_mode)) {
    return 0;
  }

  return mkdir(path, mode);
#else
  (void)mode;
  DWORD dwAttrib = GetFileAttributes(path);

  if (!(dwAttrib != INVALID_FILE_ATTRIBUTES && (dwAttrib & FILE_ATTRIBUTE_DIRECTORY))) {
    return !CreateDirectory(path, NULL);
  }

  return 0;
#endif
}


const char*
util_formatNumber(int number)
{
  static char buffer[256];
#ifdef _WIN32
  snprintf(buffer, sizeof(buffer), "%d", number);
#else
  snprintf(buffer, sizeof(buffer), "%'d", number);
#endif
  return buffer;
}


int
util_open(const char* filename, uint32_t mode)
{
#ifdef _WIN32
  return open(filename, mode|O_BINARY);
#else
  return open(filename, mode);
#endif
}


static char*
util_utf8ToLatin1(const char* buffer)
{
  iconv_t ic = iconv_open("ISO-8859-1", "UTF-8");
  size_t insize = strlen(buffer);
  char* inptr = (char*)buffer;
  size_t outsize = (insize)+1;
  char* out = calloc(1, outsize);
  char* outptr = out;
  iconv(ic, &inptr, &insize, &outptr, &outsize);
  iconv_close(ic);
  return out;
}


char*
util_latin1ToUtf8(const char* _buffer)
{
  if (_buffer) {
    iconv_t ic = iconv_open("UTF-8", "ISO-8859-1");
    char* buffer = malloc(strlen(_buffer)+1);
    if (!buffer) {
      return NULL;
    }
    strcpy(buffer, _buffer);
    size_t insize = strlen(buffer);
    char* inptr = (char*)buffer;
    size_t outsize = (insize*4)+1;
    char* out = calloc(1, outsize);
    char* outptr = out;
    iconv(ic, &inptr, &insize, &outptr, &outsize);
    iconv_close(ic);
    free(buffer);
    return out;
  }

  return NULL;
}


void
util_printFormatSpeed(int32_t size, double elapsed)
{
  double speed = (double)size/elapsed;
  if (speed < 1000) {
    printf("%0.2f b/s", speed);
  } else if (speed < 1000000) {
    printf("%0.2f kB/s", speed/1000.0f);
  } else {
    printf("%0.2f MB/s", speed/1000000.0f);
  }
}


void
util_printProgress(const char* filename, struct timeval* start, uint32_t total, uint32_t fileLength)
{
  (void)filename;
  int percentage;

  if (fileLength) {
    percentage = (((uint64_t)total*(uint64_t)100)/(uint64_t)fileLength);
  } else {
    percentage = 100;
  }

  int barWidth = main_screenWidth - 23;
  int screenPercentage = (percentage*barWidth)/100;
  struct timeval current;

#ifndef _WIN32
  printf("\r%c[K", 27);
#else
  printf("\r");
#endif
  fflush(stdout);
  if (percentage >= 100) {
    printf("\xE2\x9C\x85 "); // utf-8 tick
  } else {
    printf("\xE2\x8C\x9B "); // utf-8 hourglass
  }

  printf("%3d%% [", percentage);

  for (int i = 0; i < barWidth; i++) {
    if (screenPercentage > i) {
      printf("=");
    } else if (screenPercentage == i) {
      printf(">");
    } else {
      printf(" ");
    }
  }

  printf("] ");

  gettimeofday(&current, NULL);
  long seconds = current.tv_sec - start->tv_sec;
  long micros = ((seconds * 1000000) + current.tv_usec) - start->tv_usec;
  util_printFormatSpeed(total, ((double)micros)/1000000.0f);
#ifndef _WIN32
  fflush(stdout);
#endif
}


const char*
util_amigaBaseName(const char* filename)
{
  int i;
  for (i = strlen(filename)-1; i > 0 && filename[i] != '/' && filename[i] != ':'; --i);
  if (i > 0) {
    filename = &filename[i+1];
  }
  return filename;
}

// Static flag to prevent duplicate error messages
static int connection_error_reported = 0;

void
util_resetConnectionErrorFlag(void)
{
  connection_error_reported = 0;
}

size_t
util_recv(int socket, void *buffer, size_t length, int flags)
{
  uint32_t total = 0;
  char* ptr = buffer;
  do {
    int got = recv(socket, ptr, length-total, flags);
    if (got > 0) {
      total += got;
      ptr += got;
    } else if (got == 0) {
      // Connection closed by peer
      if (!connection_error_reported) {
        printf("Connection closed by Amiga server\n");
        connection_error_reported = 1;
      }
      return got;
    } else {
      // Error occurred (including timeout)
      if (!connection_error_reported) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
          printf("Connection timeout - Amiga may have crashed or network connection lost\n");
        } else {
          printf("Network error: %s\n", strerror(errno));
        }
        connection_error_reported = 1;
      }
      return got;
    }
  } while (total < length);

  return total;
}


int
util_sendU32(int socketFd, uint32_t data)
{
  uint32_t networkData = htonl(data);

  if (send(socketFd, (const void*)&networkData, sizeof(networkData), 0) != sizeof(networkData)) {
    return -1;
  }

  return 0;
}


int
util_recvU32(int socketFd, uint32_t *data)
{
  if (util_recv(socketFd, data, sizeof(uint32_t), 0) != sizeof(uint32_t)) {
    return -1;
  }
  *data = ntohl(*data);
  return 0;
}


int
util_recv32(int socketFd, int32_t *data)
{
  if (util_recv(socketFd, data, sizeof(int32_t), 0) != sizeof(int32_t)) {
    return -1;
  }
  *data = ntohl(*data);
  return 0;
}


int
util_sendLengthAndUtf8StringAsLatin1(int socketFd, const char* str)
{
  int error = 0;
  char* latin1 = util_utf8ToLatin1(str);
  uint32_t length = strlen(latin1);
  uint32_t networkLength = htonl(length);

  if (send(socketFd, (const void*)&networkLength, sizeof(networkLength), 0) == sizeof(networkLength)) {
    error = send(socketFd, latin1, length, 0) != (int)length;
  }

  free(latin1);
  return error;
}


char*
util_recvLatin1AsUtf8(int socketFd, uint32_t length)
{
  char* buffer = malloc(length+1);

  if (util_recv(socketFd, buffer, length, 0) != length) {
    free(buffer);
    return 0;
  }

  buffer[length] = 0;

  char* utf8 = util_latin1ToUtf8(buffer);
  free(buffer);
  return utf8;
}


const char*
util_getErrorString(uint32_t error)
{
  if (error >= sizeof(errors)) {
    error = 0;
  }

  return errors[error];
}

static void (*util_onCtrlChandler)(void) = 0;

#ifdef _WIN32
BOOL WINAPI
util_consoleHandler(DWORD signal)
{
  if (signal == CTRL_C_EVENT) {
    if (util_onCtrlChandler) {
      util_onCtrlChandler();
    }
  }
  return TRUE;
}
#else
void
util_signalHandler(int signal)
{
  if (signal == SIGINT) {
    if (util_onCtrlChandler) {
      util_onCtrlChandler();
    }
  }
}
#endif


void
util_onCtrlC(void (*handler)(void))
{
  util_onCtrlChandler = handler;
#ifdef _WIN32
  SetConsoleCtrlHandler(util_consoleHandler, TRUE);
#else
  signal(SIGINT, util_signalHandler);
#endif
}


size_t
util_strlcat(char * restrict dst, const char * restrict src, size_t maxlen)
{
  const size_t srclen = strlen(src);
  const size_t dstlen = strnlen(dst, maxlen);
  if (dstlen == maxlen) return maxlen+srclen;
  if (srclen < maxlen-dstlen) {
    memcpy(dst+dstlen, src, srclen+1);
  } else {
    memcpy(dst+dstlen, src, maxlen-1);
    dst[dstlen+maxlen-1] = '\0';
  }
  return dstlen + srclen;
}

const char*
util_getTempFolder(void)
{
  static char path[PATH_MAX];
#ifndef _WIN32
  snprintf(path, sizeof(path), "/tmp/.squirt/%d/", getpid());
  return path;
#else
  char buffer[PATH_MAX];
  GetTempPathA(PATH_MAX, buffer);
  snprintf(path, sizeof(path), "%s.squirt\\%d\\", buffer, getpid());
  return path;
#endif
}


int
util_isDirectory(const char *path)
{
#ifdef _WIN32
  struct _stat statbuf;
  if (_stat(path, &statbuf) != 0)
    return 0;
  return statbuf.st_mode & _S_IFDIR;
#else
  struct stat statbuf;
  if (stat(path, &statbuf) != 0)
    return 0;
  return S_ISDIR(statbuf.st_mode);
#endif

}


int
util_exec(char* command)
{
  char** argv = argv_build(command);
  int error = exec_cmd(argv_argc(argv), argv);
  argv_free(argv);
  return error;
}


char*
util_execCapture(char* command)
{
  char** argv = argv_build(command);
  uint32_t error = 0;
  char* output = exec_captureCmd(&error, argv_argc(argv), argv);
  argv_free(argv);
  if (error) {
    if (output) {
      free(output);
    }
    return 0;
  }
  return output;
}


int
util_system(char** argv)
{
  int argc = argv_argc(argv);
  int commandLength = 0;
  for (int i = 0; i < argc; i++) {
    if (i > 0) {
      commandLength++;
    }
    // Account for potential quotes around arguments with spaces
    int argLen = strlen(argv[i]);
    if (strchr(argv[i], ' ') != NULL) {
      commandLength += argLen + 4; // +2 for quotes, +2 for safety
    } else {
      commandLength += argLen + 2; // +2 for safety
    }
  }
  commandLength++;

  char* command = malloc(commandLength);
  command[0] = 0;
  for (int i = 0; i < argc; i++) {
    if (i > 0) {
      strcat(command, " ");
    }
    
    // Quote arguments that contain spaces, but avoid double-quoting
    if (strchr(argv[i], ' ') != NULL) {
      int argLen = strlen(argv[i]);
      // Check if argument is already quoted
      if (argLen >= 2 && 
          ((argv[i][0] == '"' && argv[i][argLen-1] == '"') ||
           (argv[i][0] == '\'' && argv[i][argLen-1] == '\''))) {
        // Already quoted, use as-is
        strcat(command, argv[i]);
      } else {
        // Not quoted, add quotes
        strcat(command, "\"");
        strcat(command, argv[i]);
        strcat(command, "\"");
      }
    } else {
      strcat(command, argv[i]);
    }
  }

#ifdef _WIN32
  // On Windows, normalize paths by converting forward slashes to backslashes
  char* normalized_command = malloc(strlen(command) + 1);
  strcpy(normalized_command, command);
  for (char* p = normalized_command; *p; p++) {
    if (*p == '/') {
      *p = '\\';
    }
  }
  
  int error = system(normalized_command);
  free(normalized_command);
#else
  int error = system(command);
#endif
  free(command);
  return error;
}


int
util_cd(const char* dir)
{
  uint32_t error = 0;

  if (util_sendCommand(main_socketFd, SQUIRT_COMMAND_CD) != 0) {
    fatalError("failed to connect to squirtd server");
  }

  if (util_sendLengthAndUtf8StringAsLatin1(main_socketFd, dir) != 0) {
    fatalError("send() command failed");
  }

  if (util_recvU32(main_socketFd, &error) != 0) {
    fatalError("cd: failed to read remote status");
  }

  return error;
}


#ifdef _WIN32
static int is_windows_reserved_name(const char* name) {
  // Convert to uppercase for case-insensitive comparison
  char upper_name[PATH_MAX];
  size_t len = strlen(name);
  size_t i;
  
  if (len >= PATH_MAX) {
    return 0; // Too long to be a reserved name
  }
  
  for (i = 0; i < len; i++) {
    upper_name[i] = toupper(name[i]);
  }
  upper_name[len] = '\0';
  
  // Check for base reserved names (exact match)
  const char* reserved_names[] = {
    "CON", "PRN", "AUX", "NUL",
    "COM1", "COM2", "COM3", "COM4", "COM5", "COM6", "COM7", "COM8", "COM9",
    "LPT1", "LPT2", "LPT3", "LPT4", "LPT5", "LPT6", "LPT7", "LPT8", "LPT9",
    NULL
  };
  
  // First check for exact matches
  for (i = 0; reserved_names[i] != NULL; i++) {
    if (strcmp(upper_name, reserved_names[i]) == 0) {
      return 1;
    }
  }
  
  // Then check for reserved names with extensions (name.ext)
  char *dot = strchr(upper_name, '.');
  if (dot) {
    *dot = '\0'; // Temporarily truncate at the dot
    for (i = 0; reserved_names[i] != NULL; i++) {
      if (strcmp(upper_name, reserved_names[i]) == 0) {
        return 1;
      }
    }
  }
  
  return 0;
}
#endif

char*
util_safeName(const char* name)
{
  // Calculate required size: original length + potential "squirt_" prefix + null terminator
  size_t safe_size = strlen(name) + 8; // "squirt_" (7) + null terminator (1)
  char* safe = malloc(safe_size);
  char* dest = safe;
  
  if (!dest) {
    return NULL;
  }
  
#ifdef _WIN32
  // Only apply Windows reserved name logic on Windows platforms
  // Check if this is a Windows reserved name
  int is_reserved = is_windows_reserved_name(name);
  
  // Add prefix for reserved names
  if (is_reserved) {
    strcpy(dest, "squirt_");
    dest += 7; // Length of "squirt_"
  }
#endif
  
  // Copy the rest of the name, preserving Windows drive letter colons
  int char_pos = 0;
  while (*name) {
    // Preserve colon if it's part of a Windows drive letter (position 1: C:, D:, etc.)
    if (*name != ':' || (char_pos == 1 && isalpha(name[-1]))) {
      *dest = *name;
      dest++;
    }
    name++;
    char_pos++;
  }
  *dest = 0;
  
  return safe;
}
