/* savestates.c: Control Mapping for OpenDingux
   Copyright (c) 2021 Pedro Luis Rodrí­guez González
   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.
   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.
   You should have received a copy of the GNU General Public License along
   with this program; if not, write to the Free Software Foundation, Inc.,
   51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
   Author contact information:
   E-mail: pl.rguez@gmail.com
*/

#include <config.h>

#include <sys/stat.h>
#include <string.h>
#include <limits.h>
#include <errno.h>
#include <mntent.h>
#include <unistd.h>
#include <libspectrum.h>
#include <ctype.h>

#include "fuse.h"
#include "snapshot.h"
#include "compat.h"
#include "utils.h"
#include "settings.h"
#include "ui/ui.h"
#include "screenshot.h"
#include "savestates/savestates.h"

#ifdef GCWZERO

static const char* re_expressions[] = {
    "(([[:space:]]|[-_])*)(([(]|[[])*[[:space:]]*)(disk|tape|side|part)(([[:space:]]|[[:punct:]])*)(([abcd1234])([[:space:]]*of[[:space:]]*[1234])*)([[:space:]]*([)]|[]])*)(([[:space:]]|[-_])*)",
    NULL };

static char* last_check_directory = NULL;
static int last_check_directory_result = 0;

static int
check_dir_exist(char* dir)
{
  struct stat stat_info;

  if( !dir ) return -1;

  /* Check if not exist */
  if( stat( dir, &stat_info ) ) {
    if ( errno == ENOENT )
      return 0;
    else
      return -1;
  }

  return 1;
}

static int
savestate_write_internal( int slot )
{
  char* filename;

  filename = quicksave_get_filename( slot );
  if ( !filename ) return 1;

  if ( quicksave_create_dir() ) return 1;

  int error = snapshot_write( filename );
  if ( error )
    ui_error( UI_ERROR_ERROR, "Error saving state to slot %02d", slot );
  else
    #ifdef MIYOO    
    ui_widget_show_msg_update_info( "Saved to slot %02d", slot );
    #else
    ui_widget_show_msg_update_info( "Saved to slot %02d (%s)", slot, get_savestate_last_change( slot ) );
    #endif

  libspectrum_free( filename );

  return error;
}

static int
savestate_read_internal( int slot )
{
  char* filename;

  /* If don't exist savestate return */
  if ( !check_current_savestate_exist( slot ) ) return 1;

  filename = quicksave_get_filename( slot );
  if ( !filename ) return 1;

  /*
   * Dirty hack for savesstates.
   * autoload is set to 9 for load from loadstates and avoid changing last
   * loaded filename and controlmapping files
   */
  int error = utils_open_file( filename, 9, NULL );
  if (error)
    ui_error( UI_ERROR_ERROR, "Error loading state from slot %02d", slot );
  else
    #ifdef MIYOO    
    ui_widget_show_msg_update_info( "Loaded slot %02d", slot );
    #else
    ui_widget_show_msg_update_info( "Loaded slot %02d (%s)", slot, get_savestate_last_change( slot ) );
    #endif

  libspectrum_free( filename );

  return error;
}

static int
is_savestate_name( const char* name )
{
  char* filename;
  char* extension;

  if ( !name ) return 0;

  /* Compare if name is valid */
  filename = utils_last_filename( name, 0 );
  if ( !filename ) return 0;

  /* nn.xxx */
  if ( strlen( filename ) != 6 ) {
    libspectrum_free( filename );
    return 0;
  }

  /* Verify that extension is the configured for savestates */
  extension = strrchr( filename, '.' );
  if ( extension ) {
    if ( strncmp( extension, settings_current.od_quicksave_format, 4 ) ) {
      libspectrum_free( filename );
      return 0;
    }
  } else {
    libspectrum_free( filename );
    return 0;
  }

  /* Verify that the name are digits */
  if ( !isdigit( filename[0] ) || !isdigit( filename[1]) ) {
    libspectrum_free( filename );
    return 0;
  }

  libspectrum_free( filename );

  /* Name is OK */
  return 1;
}

static int
scan_directory_for_savestates( const char* dir, int (*check_fn)(const char*) )
{
  compat_dir directory;
  int done = 0;
  int exist = 0;

  if ( !dir ) return 0;

  directory = compat_opendir( dir );
  if( !directory ) return 0;

  while( !done ) {
    char name[ PATH_MAX ];

    compat_dir_result_t result = compat_readdir( directory, name, sizeof( name ) );

    switch( result ) {
    case COMPAT_DIR_RESULT_OK:
      if ( check_fn( name ) ) {
        exist = 1;
        done = 1;
      }
      break;

    case COMPAT_DIR_RESULT_END:
      done = 1;
      break;

    case COMPAT_DIR_RESULT_ERROR:
      compat_closedir( directory );
      return 0;
    }
  }

  if( compat_closedir( directory ) )
    return 0;

  return exist;
}

static int
savetate_read_snapshot( char* snapshot, libspectrum_snap *snap, utils_file* file )
{
  int error;

  if ( !snapshot || !compat_file_exists( snapshot ))
    return 1;

  error = utils_read_file( snapshot, file );
  if( error )
    return error;

  error = libspectrum_snap_read( snap, file->buffer, file->length,
                         LIBSPECTRUM_ID_UNKNOWN, snapshot );
  if( error ) {
    utils_close_file( file );
    return error;
  }

  utils_close_file( file );

  return 0;
}

int
savestate_get_screen_for_slot( int slot, utils_file* screen )
{
  utils_file file;
  char* savestate;
  int error;
  libspectrum_snap *snap;
  int page;

  /* Initialize screenshot to black */
  screen->length = 6912;
  screen->buffer = libspectrum_new( unsigned char, screen->length );
  memset( screen->buffer, 0, screen->length );

  savestate = quicksave_get_filename( slot );

  snap = libspectrum_snap_alloc();

  error = savetate_read_snapshot( savestate, snap, &file );

  libspectrum_free( savestate );

  if( error ) {
    libspectrum_snap_free( snap );
    return error;
  }

  switch ( libspectrum_snap_machine( snap ) ) {
  case LIBSPECTRUM_MACHINE_PENT:
  case LIBSPECTRUM_MACHINE_PENT512:
  case LIBSPECTRUM_MACHINE_PENT1024:
  case LIBSPECTRUM_MACHINE_SCORP:
  case LIBSPECTRUM_MACHINE_PLUS3E:
  case LIBSPECTRUM_MACHINE_PLUS2A:
  case LIBSPECTRUM_MACHINE_PLUS3:
  case LIBSPECTRUM_MACHINE_PLUS2:
  case LIBSPECTRUM_MACHINE_128:
  case LIBSPECTRUM_MACHINE_128E:
  case LIBSPECTRUM_MACHINE_SE:
    page = libspectrum_snap_out_128_memoryport( snap ) & 0x08 ? 7 : 5;
    break;
  default:
    page = 5;
    break;
  }
  memcpy(screen->buffer, libspectrum_snap_pages( snap, page ), screen->length);

  error = libspectrum_snap_free( snap );
  if( error )
    return error;

  return 0;
}

char*
quicksave_get_current_dir(void)
{
  const char* cfgdir;
  char buffer[ PATH_MAX ];
  char* filename;

  if ( !last_filename ) return NULL;

  /* Don't exist config path, no error but do nothing */
  cfgdir = compat_get_config_path(); if( !cfgdir ) return NULL;

  filename = quicksave_get_current_program();

  if (settings_current.od_quicksave_per_machine) {
      snprintf( buffer, PATH_MAX, "%s"FUSE_DIR_SEP_STR"%s"FUSE_DIR_SEP_STR"%s"FUSE_DIR_SEP_STR"%s",
                cfgdir, "savestates", libspectrum_machine_name( machine_current->machine ), filename );
  } else {
    snprintf( buffer, PATH_MAX, "%s"FUSE_DIR_SEP_STR"%s"FUSE_DIR_SEP_STR"%s",
              cfgdir, "savestates", filename );
  }

  libspectrum_free( filename );

  return utils_safe_strdup( buffer );
}

int
quicksave_create_dir(void)
{
  char* savestate_dir;

  /* Can not determine savestate_dir */
  savestate_dir = quicksave_get_current_dir();
  if( !savestate_dir ) return 1;

  /* Create if don't exist */
  int exist = check_dir_exist( savestate_dir );
  if( !exist ) {
    if ( compat_createdir( savestate_dir ) == -1 ) {
      ui_error( UI_ERROR_ERROR, "error creating savestate directory '%s'", savestate_dir );
      return 1;
    }
  } else if ( exist == -1 ) {
     ui_error( UI_ERROR_ERROR, "couldn't stat '%s': %s", savestate_dir, strerror( errno ) );
     return 1;
  }

  libspectrum_free( savestate_dir );

  return 0;
}

char*
quicksave_get_current_program(void)
{
  if ( !last_filename ) return NULL;

  return compat_chop_expressions( re_expressions, utils_last_filename( last_filename, 1 ) );
}

char*
quicksave_get_label(int slot)
{
  char* current_program;
  char* filename;
  char* label;

  current_program = quicksave_get_current_program();
  if ( !current_program ) return NULL;

  filename = utils_last_filename( quicksave_get_filename(slot), 1 );
  if ( !filename ) {
    libspectrum_free(current_program);
    return NULL;
  }

  label = libspectrum_new(char, 20);
  snprintf(label,20,"%s: %s",filename,current_program);
  if ( strlen(current_program) > 15 )
    memcpy( &(label[18]), ">\0", 2 );

  libspectrum_free(filename);
  libspectrum_free(current_program);

  return label;
}

int
check_current_savestate_exist(int slot)
{
  char* filename;
  int exist = 0;

  filename = quicksave_get_filename(slot);
  if (filename) {
    exist = compat_file_exists( filename ) ? 1 : 0;
    libspectrum_free(filename);
  }

  return exist;
}

int
check_current_savestate_exist_savename( char* savename )
{
  int slot;
  char* cslot;

  if (!savename) return 0;

  cslot = utils_last_filename( savename, 1 );
  slot = atoi( cslot );
  libspectrum_free( cslot );

  return check_current_savestate_exist( slot );
}

int
check_any_savestate_exist(void)
{
  char* savestate_dir;

  /* Can not determine savestate_dir */
  savestate_dir = quicksave_get_current_dir();
  if( !savestate_dir ) return 0;

  if ( !check_dir_exist( savestate_dir ) ) {
    libspectrum_free(savestate_dir);
    return 0;
  }

  /* Is directory previously checked? */
  if ( !last_check_directory || strcmp( savestate_dir, last_check_directory ) ) {
    last_check_directory = utils_safe_strdup( savestate_dir );
    last_check_directory_result = 0;
  /* If directory contain savestates previously */
  } else if ( !last_check_directory_result ) {
    last_check_directory_result = scan_directory_for_savestates( savestate_dir, is_savestate_name );
  }

  libspectrum_free( savestate_dir );

  return last_check_directory_result;
}

int
check_if_savestate_possible(void)
{
  char* current_program;
  int possible = 0;

  current_program = quicksave_get_current_program();
  if (current_program) {
    possible = 1;
    libspectrum_free( current_program );
  }
  return possible;
}

char*
quicksave_get_filename(int slot)
{
  char *current_dir;
  char buffer[ PATH_MAX ];

  current_dir = quicksave_get_current_dir();
  if ( !current_dir ) return NULL;

  snprintf( buffer, PATH_MAX, "%s"FUSE_DIR_SEP_STR"%02d%s",
            current_dir, slot, settings_current.od_quicksave_format );

  libspectrum_free( current_dir );

  return utils_safe_strdup( buffer );
}

char*
get_savestate_last_change(int slot) {
  compat_fd save_state_fd;
  char* last_change;
  char* filename;

  if ( !check_current_savestate_exist( slot ) )
    return NULL;

  filename = quicksave_get_filename( slot );
  save_state_fd = compat_file_open( filename, 0 );
  libspectrum_free( filename );
  if ( !save_state_fd )
    return NULL;

  last_change = compat_file_get_time_last_change( save_state_fd );
  compat_file_close( save_state_fd );

  /* Get rid of \n */
  if ( last_change )
    last_change[ strlen( last_change ) - 1 ] = '\0';

  return last_change;
}

int
quicksave_load(void)
{
  /* If don't exist savestate return but don't mark error */
  if ( !check_current_savestate_exist( settings_current.od_quicksave_slot ) )
    return 0;

  fuse_emulation_pause();

  int error = savestate_read_internal( settings_current.od_quicksave_slot );

  display_refresh_all();

  fuse_emulation_unpause();

  return error;
}

int
quicksave_save(void)
{
  fuse_emulation_pause();

  int error = savestate_write_internal( settings_current.od_quicksave_slot );

  fuse_emulation_unpause();

  return error;
}

int
savestate_read( const char *savestate )
{
  char* slot;

  slot = utils_last_filename( savestate, 1 );
  if ( !slot ) return 1;

  settings_current.od_quicksave_slot = atoi( slot );

  libspectrum_free( slot );

  return savestate_read_internal( settings_current.od_quicksave_slot );
}

int
savestate_write( const char *savestate )
{
  char* slot;

  slot = utils_last_filename( savestate, 1 );
  if ( !slot ) return 1;

  settings_current.od_quicksave_slot = atoi( slot );

  libspectrum_free(slot);

  return savestate_write_internal( settings_current.od_quicksave_slot );
}
#endif /* #ifdef GCWZERO */