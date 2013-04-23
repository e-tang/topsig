#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include "topsig-index.h"
#include "topsig-file.h"
#include "topsig-filerw.h"
#include "topsig-config.h"
#include "topsig-process.h"
#include "topsig-thread.h"
#include "topsig-signature.h"
#include "topsig-stats.h"
#include "topsig-global.h"
#include "topsig-document.h"
#include "uthash.h"

#define BUFFER_SIZE (512 * 1024)

static char current_archive_path[2048];

typedef struct {
  char from[256];
  char to[256];
  UT_hash_handle hh;
} docid_mapping;

docid_mapping *docid_mapping_list = NULL;
docid_mapping *docid_mapping_hash = NULL;

static char *DocumentID(char *path, char *data)
{
  char *docid = NULL;
  if (lc_strcmp(Config("DOCID-FORMAT"), "path")==0) {
    docid = malloc(strlen(path)+1);
    strcpy(docid, path);
  } else if (lc_strcmp(Config("DOCID-FORMAT"), "basename.ext")==0) {
    char *p = strrchr(path, '/');
    if (p == NULL)
      p = path;
    else
      p = p + 1;
    docid = malloc(strlen(p)+1);
    strcpy(docid, p);
  } else if (lc_strcmp(Config("DOCID-FORMAT"), "basename")==0) {
    char *p = strrchr(path, '/');
    if (p == NULL)
      p = path;
    else
      p = p + 1;
    docid = malloc(strlen(p)+1);
    strcpy(docid, p);
    p = strrchr(docid, '.');
    if (p) *p = '\0';
  } else if (lc_strcmp(Config("DOCID-FORMAT"), "xmlfield")==0) {
    char *docid_field = Config("XML-DOCID-FIELD");
    if (!docid_field) {
      fprintf(stderr, "DOCID-FORMAT=xmlfield but XML-DOCID-FIELD unspecified\n");
      exit(1);
    }
    
    char xml_open[256], xml_close[256];
    sprintf(xml_open, "<%s>", docid_field);
    sprintf(xml_close, "</%s>", docid_field);
    
    char *start = strstr(data, xml_open);
    char *end = strstr(data, xml_close);
    if (!start || !end) {
      // XML field not found. Use path
      docid = malloc(strlen(path)+1);
      strcpy(docid, path);
    } else {
      start += strlen(xml_open);
      docid = malloc(end - start + 1);
      memcpy(docid, start, end - start);
      docid[end - start] = '\0';
    }
  } else {
    fprintf(stderr, "DOCID-FORMAT invalid.\n");
    exit(1);
  }
  
  if (docid_mapping_hash) {
    docid_mapping *lookup;
    HASH_FIND_STR(docid_mapping_hash, docid, lookup);
    //printf("Lookup %s...\n", docid);
    if (lookup) {
      //printf("%s->%s\n", lookup->from, lookup->to);
      docid = realloc(docid, strlen(lookup->to)+1);
      strcpy(docid, lookup->to);
    }
  }
  
  return docid;
}

// Get the next file pointed to
static char *getnextfile(char *path)
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

static void indexfile(Document *doc)
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
    ProcessFile(signaturecache, doc);
  } else {
    ProcessFile_Threaded(doc);
  }
}

static void AR_file(FileHandle *fp, void (*processfile)(Document *))
{
  int filesize = 0;
  int buffersize = 1024;
  Document *doc = NewDocument(NULL, NULL);
  
  char *filedat = NULL;  
  
  for (;;) {
    filedat = realloc(filedat, buffersize);
    int rbuf = file_read(filedat+filesize, buffersize-filesize, fp);
    filesize += rbuf;
    if (rbuf == 0) break;
    
    buffersize *= 2;
  }
  
  filedat[filesize] = '\0';
  doc->data = filedat;
  doc->data_length = filesize;
  doc->docid = DocumentID(current_archive_path, filedat);
  
  processfile(doc);
}


#define WARC_BUFFER 65536
static int warc_fillbuffer(FileHandle *fp, char *buffer, int *buffer_filled) {
  int bspace = WARC_BUFFER - *buffer_filled;
  int eof = 0;
  int octets_read = file_read(buffer + *buffer_filled, bspace, fp);
  for (int i = 0; i < octets_read; i++) {
    if (buffer[*buffer_filled+i] == '\0') buffer[*buffer_filled+i] = '_';
  }
  if (octets_read < bspace) {
    eof = 1;
  }
  *buffer_filled = *buffer_filled + octets_read;
  
  return eof;
}

static void warc_erasebuffer(char *buffer, int *buffer_filled, int erase_bytes)
{
  // Erase 'erase_bytes' bytes from the buffer, moving the remaining portion
  // to the start.
  memmove(buffer, buffer + erase_bytes, WARC_BUFFER - erase_bytes);
  *buffer_filled -= erase_bytes;
}

static void AR_warc(FileHandle *fp, void (*processfile)(Document *))
{
  char buffer[WARC_BUFFER];
  char header[WARC_BUFFER];
  int buffer_filled = 0;
  int eof = 0;
  // WARC files consist of records with headers, containing several named fields
  // and ending in two newlines. 
  
  while (!(eof && (buffer_filled == 0))) {
    eof = warc_fillbuffer(fp, buffer, &buffer_filled);
    char *header_end = strstr(strstr(buffer, "Content-Length:"), "\n\n");
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
      fprintf(stderr, "WARC format error B\n");
      exit(1);
    }
    pos = strstr(header, "Content-Length:");
    if (pos) {
      sscanf(pos, "%s %d", fieldname, &Content_Length);
    } else {
      fprintf(stderr, "WARC format error C\n");
      exit(1);
    }
    
    pos = strstr(header, "WARC-TREC-ID:");
    if (pos) {
      sscanf(pos, "%s %s", fieldname, WARC_TREC_ID);
    }
    
    warc_erasebuffer(buffer, &buffer_filled, header_size);
    
    Document *newDoc = NewDocument(WARC_TREC_ID, NULL);
    newDoc->data = malloc(Content_Length + 1);
    int filedat_size = 0;
    
    while (filedat_size < Content_Length) {
      int buffer_copy = buffer_filled;
      if (Content_Length - filedat_size < buffer_copy) {
        buffer_copy = Content_Length - filedat_size;
      }
      memcpy(newDoc->data+filedat_size, buffer, buffer_copy);
      warc_erasebuffer(buffer, &buffer_filled, buffer_copy);
      filedat_size += buffer_copy;
      
      eof = warc_fillbuffer(fp, buffer, &buffer_filled);
    }
    newDoc->data[filedat_size] = '\0';
    
    if (lc_strcmp(WARC_Type, "response")==0) {
      //printf("Index [%s]\n", newDoc->docid);
      processfile(newDoc);
    } else {
      FreeDocument(newDoc);
    }
  }
}

static void AR_tar(FileHandle *fp, void (*processfile)(Document *))
{ 
  for (;;) {
    char buffer[512];
    int rlen = file_read(buffer, 512, fp);
    if (rlen < 512) break;
        
    int file_size;
    sscanf(buffer+124, "%o", &file_size);
    
    char *filedat = malloc(file_size + 1);
    for (int file_offset = 0; file_offset < file_size; file_offset += 512) {
      char buffer[512];
      file_read(buffer, 512, fp);
      int blocklen = file_size - file_offset;
      if (blocklen > 512) blocklen = 512;
      
      memcpy(filedat + file_offset, buffer, blocklen);
    }
    filedat[file_size] = '\0';
    char *filename = DocumentID(buffer, filedat);
    Document *newDoc = NewDocument(NULL, NULL);
    newDoc->data = filedat;
    newDoc->data_length = file_size;
    newDoc->docid = filename;
    
    if (strcmp(filename, "NULL")==0) {
      FreeDocument(newDoc);
    } else {
      processfile(newDoc);
    }
  }
}

static void AR_wsj(FileHandle *fp,  void (*processfile)(Document *))
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
        
        Document *newDoc = NewDocument(NULL, NULL);
        newDoc->docid = filename;
        newDoc->data = filedat;
        newDoc->data_length = archiveSize;
        
        processfile(newDoc);
                
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

static void AR_newline(FileHandle *fp,  void (*processfile)(Document *))
{
  int archiveSize;
  char *doc_start;
  char *doc_end;

  char buf[BUFFER_SIZE];
  
  int buflen = file_read(buf, BUFFER_SIZE-1, fp);
  int doclen;
  buf[buflen] = '\0';
  int file_index = 0;
  
  for (;;) {
    if ((doc_start = strstr(buf, "\n")) != NULL) {
      if ((doc_end = strstr(doc_start+1, "\n")) != NULL) {
        file_index++;
        doclen = doc_end-buf;
        
        char *filename = malloc(8);
        sprintf(filename, "%04d", file_index);
                
        archiveSize = doc_end-doc_start;

        char *filedat = malloc(archiveSize + 1);
        memcpy(filedat, doc_start, archiveSize);
        filedat[archiveSize] = '\0';
        
        Document *newDoc = NewDocument(NULL, NULL);
        newDoc->docid = filename;
        newDoc->data = filedat;
        newDoc->data_length = archiveSize;
        
        processfile(newDoc);
                
        memmove(buf, doc_end, buflen-doclen);
        buflen -= doclen;
        
        buflen += file_read(buf+buflen, BUFFER_SIZE-1-buflen, fp);
        buf[buflen] = '\0';
      } else {
        break;
      }
    } else {
      break;
    }
  }
}


// Reader for the Khresmoi medical documents 2012 web crawl (and possibly other similar crawls)
static void AR_khresmoi(FileHandle *fp,  void (*processfile)(Document *))
{
  int archiveSize;
  char *filename_start;
  char *doc_start;
  char *doc_end;

  char buf[BUFFER_SIZE];
  
  int buflen = file_read(buf, BUFFER_SIZE-1, fp);
  int doclen;
  buf[buflen] = '\0';
  int file_index = 0;
  
  for (;;) {
    if ((filename_start = strstr(buf, "#UID:")) != NULL) {
      if ((doc_start = strstr(filename_start+1, "#CONTENT:")) != NULL) {
        if ((doc_end = strstr(doc_start+1, "\n#EOR")) != NULL) {
          file_index++;
          doclen = doc_end-buf;
          
          doc_start += strlen("#CONTENT:");
          
          filename_start += strlen("#UID:");
          char *filename_end = strchr(filename_start, '\n');
          
          int filename_len = filename_end - filename_start;
          char *filename = malloc(filename_len + 1);
          memcpy(filename, filename_start, filename_len);
          filename[filename_len] = '\0';
                  
          archiveSize = doc_end-doc_start;

          char *filedat = malloc(archiveSize + 1);
          memcpy(filedat, doc_start, archiveSize);
          filedat[archiveSize] = '\0';
          
          Document *newDoc = NewDocument(NULL, NULL);
          newDoc->docid = filename;
          newDoc->data = filedat;
          newDoc->data_length = archiveSize;
          
          processfile(newDoc);
                  
          memmove(buf, doc_end, buflen-doclen);
          buflen -= doclen;
          
          buflen += file_read(buf+buflen, BUFFER_SIZE-1-buflen, fp);
          buf[buflen] = '\0';
        } else {
          break;
        }
      } else {
        break;
      }
    } else {
      break;
    }
  }
}


static void (*getarchivereader(const char *targetformat))(FileHandle *, void (*)(Document *))
{
  void (*archivereader)(FileHandle *, void (*)(Document *)) = NULL;
  if (lc_strcmp(targetformat, "file")==0) archivereader = AR_file;
  if (lc_strcmp(targetformat, "tar")==0) archivereader = AR_tar;
  if (lc_strcmp(targetformat, "wsj")==0) archivereader = AR_wsj;
  if (lc_strcmp(targetformat, "warc")==0) archivereader = AR_warc;
  if (lc_strcmp(targetformat, "newline")==0) archivereader = AR_newline;
  if (lc_strcmp(targetformat, "khresmoi")==0) archivereader = AR_khresmoi;
  return archivereader;
}

void RunIndex()
{
  char path[2048];
  void (*archivereader)(FileHandle *, void (*)(Document *));
  archivereader = getarchivereader(Config("TARGET-FORMAT"));

  if (archivereader == NULL) {
    fprintf(stderr, "Invalid/unspecified TARGET-FORMAT\n");
    exit(1);
  }
  
  while (getnextfile(path)) {
    //printf("%s\n", path);
    FileHandle *fp = file_open(path);
    if (fp) {
      strcpy(current_archive_path, path);
      archivereader(fp, indexfile);
      file_close(fp);
    }
  }
  Flush_Threaded();
}

static void addstats(Document *doc)
{
  ProcessFile(NULL, doc);
}

void RunTermStats()
{
  char path[2048];
  void (*archivereader)(FileHandle *, void (*)(Document *));
  archivereader = getarchivereader(Config("TARGET-FORMAT"));

  if (archivereader == NULL) {
    fprintf(stderr, "Invalid/unspecified TARGET-FORMAT\n");
    exit(1);
  }
  
  while (getnextfile(path)) {
    //printf("%s\n", path);
    FileHandle *fp = file_open(path);
    if (fp) {
      strcpy(current_archive_path, path);
      archivereader(fp, addstats);
      file_close(fp);
    }
  }
  WriteStats();
}

void Index_InitCfg()
{
  char *C = Config("MEDTRACK-MAPPING-FILE");
  char *T = Config("MEDTRACK-MAPPING-TYPE");
  if (C) {
    FILE *fp = fopen(C, "r");
    int records = atoi(Config("MEDTRACK-MAPPING-RECORDS"));
    docid_mapping_list = malloc(sizeof(docid_mapping) * records);
    int recordnum = 0;
    for (int i = 0; i < records; i++) {
      char from[1024];
      char to[1024];
      char rectype[1024];
      fscanf(fp, "%s %s %s\n", from, rectype, to);
      
      int process_record = 1;
      if (T && strstr(T, rectype)==NULL) {
        process_record = 0;
      }
      
      docid_mapping *newrecord = docid_mapping_list+recordnum;
      strcpy(newrecord->from, from);
      if (process_record) {
        strcpy(newrecord->to, to);
      } else {
        strcpy(newrecord->to, "NULL");
      }
      HASH_ADD_STR(docid_mapping_hash, from, newrecord);
      recordnum++;

    }
    fclose(fp);
  }
}
