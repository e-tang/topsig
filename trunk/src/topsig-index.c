#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "topsig-file.h"
#include "topsig-filerw.h"
#include "topsig-config.h"
#include "topsig-process.h"
#include "topsig-thread.h"
#include "topsig-signature.h"
#include "topsig-global.h"

#define BUFFER_SIZE (512 * 1024) 

static char current_archive_path[2048];

char *DocumentID(char *path)
{
  char *docid = malloc(strlen(path)+1);
  if (lc_strcmp(Config("DOCID-FORMAT"), "path")==0) {
    strcpy(docid, path);
  } else if (lc_strcmp(Config("DOCID-FORMAT"), "basename.ext")==0) {
    char *p = strrchr(path, '/');
    if (p == NULL)
      p = path;
    else
      p = p + 1;
    strcpy(docid, p);
  } else if (lc_strcmp(Config("DOCID-FORMAT"), "basename")==0) {
    char *p = strrchr(path, '/');
    if (p == NULL)
      p = path;
    else
      p = p + 1;
    strcpy(docid, p);
    p = strrchr(docid, '.');
    if (p) *p = '\0';
  } else {
    fprintf(stderr, "DOCID-FORMAT invalid.\n");
    exit(1);
  }
  return docid;
}

// Get the next file pointed to
char *getnextfile(char *path)
{
  // In the configuration, both files and directories can be specified.
  static int cfg_pos = 0;
  static DIR *curr_dir = NULL;
  static char *curr_dir_path = NULL;
  
  // Loop until we return something
  for (;;) {
  
    if (curr_dir) {
      struct dirent *dir_ent = readdir(curr_dir);
      if (dir_ent) {
        if (strcmp(dir_ent->d_name, ".")==0) continue;
        if (strcmp(dir_ent->d_name, "..")==0) continue;
        sprintf(path, "%s%s%s", curr_dir_path, getfileseparator(), dir_ent->d_name);
        return path;
      }
      closedir(curr_dir);
      curr_dir = NULL;
    }
    cfg_pos++;
    
    char cfg_opt[128];
    if (cfg_pos == 1) {
      sprintf(cfg_opt, "TARGET-PATH");
    } else {
      sprintf(cfg_opt, "TARGET-PATH-%d", cfg_pos);
    }
    char *fpath = Config(cfg_opt);
    
    if (fpath == NULL) return NULL;
    if (fpath[0] != '\0') {
      if (is_directory(fpath)) { // Directory
        curr_dir_path = fpath;
        curr_dir = opendir(fpath);
      } else {
        strcpy(path, fpath);
        return path;
      }
    }
  }
  
  return NULL;
}

void indexfile(char *filename, char *filedat)
{
  static int thread_mode = -1;
  static SignatureCache *signaturecache = NULL;
  
  if (thread_mode == -1) {
    if (Config("INDEX-THREADING") && strcmp(Config("INDEX-THREADING"), "multi")==0) {
      thread_mode = 1;
    } else {
      thread_mode = 0;
    }
  }
  
  if (thread_mode == 0) { // Single-threaded
    if (signaturecache == NULL) {
      signaturecache = NewSignatureCache(1, 1);
    }
    ProcessFile(signaturecache, filename, filedat);
  } else {
    ProcessFile_Threaded(filename, filedat);
  }
}

void AR_file(FileHandle *fp)
{
  int filesize = 0;
  int buffersize = 1024;
  
  char *filedat = NULL;  
  
  for (;;) {
    filedat = realloc(filedat, buffersize);
    int rbuf = file_read(filedat+filesize, buffersize-filesize, fp);
    filesize += rbuf;
    if (rbuf == 0) break;
    
    buffersize *= 2;
  }
  
  char *docid = DocumentID(current_archive_path);
  filedat[filesize] = '\0';
  
  indexfile(docid, filedat);
}


#define WARC_BUFFER 4096
int warc_fillbuffer(FileHandle *fp, char *buffer, int *buffer_filled) {
  int bspace = WARC_BUFFER - *buffer_filled;
  int eof = 0;
  int octets_read = file_read(buffer + *buffer_filled, bspace, fp);
  if (octets_read < bspace) {
    eof = 1;
  }
  *buffer_filled = *buffer_filled + octets_read;
  
  return eof;
}

void warc_erasebuffer(char *buffer, int *buffer_filled, int erase_bytes)
{
  // Erase 'erase_bytes' bytes from the buffer, moving the remaining portion
  // to the start.
  memmove(buffer, buffer + erase_bytes, WARC_BUFFER - erase_bytes);
  *buffer_filled -= erase_bytes;
}

void AR_warc(FileHandle *fp)
{
  char buffer[WARC_BUFFER];
  char header[WARC_BUFFER];
  int buffer_filled = 0;
  int eof = 0;
  // WARC files consist of records with headers, containing several named fields
  // and ending in two newlines. 
  
  while (!(eof && (buffer_filled == 0))) {
    eof = warc_fillbuffer(fp, buffer, &buffer_filled);
    char *header_end = strstr(buffer, "\n\n");
    if (header_end == NULL) {
      fprintf(stderr, "WARC format error\n");
      exit(1);
    }
    
    header_end += 2;
    size_t header_size = header_end - buffer;
    strncpy(header, buffer, header_size);
    header[header_size] = '\0';
    
    // WARC records have plenty of records, the ones we care about are
    // 'WARC-Type', 'WARC-TREC-ID' and 'Content-Length'
    
    char WARC_Type[256] = "";
    char WARC_TREC_ID[256] = "";
    char fieldname[256];
    int Content_Length = -1;
    char *pos;
    pos = strstr(header, "WARC-Type:");
    if (pos) {
      sscanf(pos, "%s %s", fieldname, WARC_Type);
    } else {
      fprintf(stderr, "WARC format error\n");
      exit(1);
    }
    pos = strstr(header, "Content-Length:");
    if (pos) {
      sscanf(pos, "%s %d", fieldname, &Content_Length);
    } else {
      fprintf(stderr, "WARC format error\n");
      exit(1);
    }
    
    pos = strstr(header, "WARC-TREC-ID:");
    if (pos) {
      sscanf(pos, "%s %s", fieldname, WARC_TREC_ID);
    }
    
    warc_erasebuffer(buffer, &buffer_filled, header_size);
    
    char *filename = malloc(strlen(WARC_TREC_ID) + 1);
    strcpy(filename, WARC_TREC_ID);
    char *filedat = malloc(Content_Length + 1);
    int filedat_size = 0;
    
    while (filedat_size < Content_Length) {
      int buffer_copy = buffer_filled;
      if (Content_Length - filedat_size < buffer_copy) {
        buffer_copy = Content_Length - filedat_size;
      }
      memcpy(filedat+filedat_size, buffer, buffer_copy);
      warc_erasebuffer(buffer, &buffer_filled, buffer_copy);
      filedat_size += buffer_copy;
      
      eof = warc_fillbuffer(fp, buffer, &buffer_filled);
    }
    filedat[filedat_size] = '\0';
    
    if (lc_strcmp(WARC_Type, "response")==0) {
      //printf("Index [%s]\n", filename);
      indexfile(filename, filedat);
    } else {
      free(filename);
      free(filedat);
    }
  }
}

void AR_tar(FileHandle *fp)
{
  char buffer[512];
  
  for (;;) {
    int rlen = file_read(buffer, 512, fp);
    if (rlen < 512) break;
    
    char *filename = DocumentID(buffer);
    
    int file_size;
    sscanf(buffer+124, "%i", &file_size);
    
    char *filedat = malloc(file_size + 1);
    for (int file_offset = 0; file_offset < file_size; file_offset += 512) {
      file_read(buffer, 512, fp);
      int blocklen = file_size - file_offset;
      if (blocklen > 512) blocklen = 512;
      
      memcpy(filedat + file_offset, buffer, blocklen);
    }
    filedat[file_size] = '\0';
    indexfile(filename, filedat);
  }
}

void AR_wsj(FileHandle *fp)
{
  int archiveSize;
  char *doc_start;
  char *doc_end;

  char buf[BUFFER_SIZE];
  
  int buflen = file_read(buf, BUFFER_SIZE-1, fp);
  int doclen;
  buf[buflen] = '\0';
  
  for (;;) {
    if ((doc_start = strstr(buf, "<DOC>")) != NULL) {
      if ((doc_end = strstr(buf, "</DOC>")) != NULL) {
        doc_end += 7;
        doclen = doc_end-buf;
        //printf("Document found, %d bytes large\n", doclen);
        
        char *title_start = strstr(buf, "<DOCNO>");
        char *title_end = strstr(buf, "</DOCNO>");
        
        title_start += 1;
        title_end -= 1;
        
        title_start += 7;
        
        int title_len = title_end - title_start;
        char *filename = malloc(title_len + 1);
        memcpy(filename, title_start, title_len);
        filename[title_len] = '\0';
                
        archiveSize = doc_end-doc_start;

        char *filedat = malloc(archiveSize + 1);
        memcpy(filedat, doc_start, archiveSize);
        filedat[archiveSize] = '\0';
        
        indexfile(filename, filedat);
                
        memmove(buf, doc_end, buflen-doclen);
        buflen -= doclen;
        
        buflen += file_read(buf+buflen, BUFFER_SIZE-1-buflen, fp);
        buf[buflen] = '\0';
      }
    } else {
      break;
    }
  }
}


void RunIndex()
{
  char path[2048];
  void (*archivereader)(FileHandle *) = NULL;
  char *targetformat = Config("TARGET-FORMAT");
  if (lc_strcmp(targetformat, "file")==0) archivereader = AR_file;
  if (lc_strcmp(targetformat, "tar")==0) archivereader = AR_tar;
  if (lc_strcmp(targetformat, "wsj")==0) archivereader = AR_wsj;
  if (lc_strcmp(targetformat, "warc")==0) archivereader = AR_warc;
  
  if (archivereader == NULL) {
    fprintf(stderr, "Invalid/unspecified TARGET-FORMAT\n");
    exit(1);
  }
  
  while (getnextfile(path)) {
    //printf("%s\n", path);
    FileHandle *fp = file_open(path);
    if (fp) {
      strcpy(current_archive_path, path);
      archivereader(fp);
      file_close(fp);
    }
  }
  Flush_Threaded();
}
