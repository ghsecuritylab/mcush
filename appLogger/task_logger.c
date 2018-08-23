/* MCUSH designed by Peng Shulin, all rights reserved. */
#include <stdarg.h>
#include "mcush.h"
#include "task_logger.h"
#include "task_blink.h"


//#define DEBUG_LOGGER  1

QueueHandle_t queue_logger;
QueueHandle_t queue_logger_monitor;
static uint8_t monitoring_mode=0;

static const char _fname[] = LOGGER_FNAME;
static uint8_t _enable = LOGGER_ENABLE;

 
void logger_enable(void)
{
    _enable = 1;
}


void logger_disable(void)
{
    _enable = 0;
}


int logger_is_enabled(void)
{
    return _enable;
}


char *convert_logger_event_to_str( logger_event_t *evt, char *buf )
{
    char tp[2];

#if HAL_RTC
    if( ! get_rtc_str( buf ) )
        get_uptime_str( buf, 1 ); 
#else
    get_uptime_str( buf, 1 ); 
#endif
    strcat( buf, " " );
    if( evt->type & LOG_ERROR )
        tp[0] = _enable ? 'E' : 'e';
    else if( evt->type & LOG_WARN )
        tp[0] = _enable ? 'W' : 'w';
    else if( evt->type & LOG_INFO )
        tp[0] = _enable ? 'I' : 'i';
    else if( evt->type & LOG_DEBUG )
        tp[0] = _enable ? 'D' : 'd';
    else
        tp[0] = 0;
    tp[1] = 0;
    strcat( buf, tp );
    strcat( buf, " " );
    strcat( buf, evt->str );
    strcat( buf, "\n" );
    return buf;    
}

 
int in_logger_str_malloc=0;
static int _logger_str( int type, const char *str, int isr_mode )
{
    logger_event_t evt;
    uint32_t length = strlen(str);
    char *buf;
    int err=0;
    portBASE_TYPE xHigherPriorityTaskWoken = pdFALSE;
    
    in_logger_str_malloc=1;
    buf = (char*)malloc(length+1);
    in_logger_str_malloc=0;
    if( buf == NULL )
        return 0;

    evt.type = type;
    evt.time = (uint32_t)xTaskGetTickCount();
    evt.str = buf;
    strncpy( buf, str, length+1 );
    if( isr_mode )
    {
        if( xQueueSendFromISR( queue_logger, &evt, &xHigherPriorityTaskWoken ) != pdTRUE )
            err = 1;
        else
            portEND_SWITCHING_ISR(xHigherPriorityTaskWoken);
    }
    else
    { 
        if( xQueueSend( queue_logger, &evt, 0 ) != pdTRUE )
            err = 1;
    }

    if( err )
        free( (void*)buf );
    return err ? 0 : 1;
}

/* isr APIs */
int logger_str_isr( int type, const char *str )
{
    return _logger_str( type, str, 1 );
}

int logger_debug_isr( const char *str )
{
    return _logger_str( LOG_DEBUG, str, 1 );
}

int logger_info_isr( const char *str )
{
    return _logger_str( LOG_INFO, str, 1 );
}

int logger_warn_isr( const char *str )
{
    return _logger_str( LOG_WARN, str, 1 );
}

int logger_error_isr( const char *str )
{
    return _logger_str( LOG_ERROR, str, 1 );
}

/* normal APIs */
int logger_str( int type, const char *str )
{
    return _logger_str( type, str, 0 );
}

int logger_debug( const char *str )
{
    return _logger_str( LOG_DEBUG, str, 0 );
}

int logger_info( const char *str )
{
    return _logger_str( LOG_INFO, str, 0 );
}

int logger_warn( const char *str )
{
    return _logger_str( LOG_WARN, str, 0 );
}

int logger_error( const char *str )
{
    return _logger_str( LOG_ERROR, str, 0 );
}

/* printf APIs */
int logger_printf( int type, char *fmt, ... )
{
    va_list ap;
    int n;
    char buf[256];

    va_start( ap, fmt );
    n = vsprintf( buf, fmt, ap );
    va_end( ap );
    return logger_str( type, buf );
}


static char *_join_log_fname( char *buf, int level )
{
    if( level > 0 )
        sprintf( buf, "%s.%d", _fname, level );
    else
        strcpy( buf, _fname );
    return buf;
}


int delete_all_log_files( void )
{
    char fname[20];
    int i=0;
    /* remove all */
    for( i=0; i<=LOGGER_ROTATE_LEVEL; i++ )
    {
        _join_log_fname(fname, i);
        mcush_remove( fname );
#if DEBUG_LOGGER
        shell_printf( "remove %s\n", fname );
#endif
    }

    /* verify */
    for( i=0; i<=LOGGER_ROTATE_LEVEL; i++ )
    {
        if( mcush_file_exists( _join_log_fname(fname, i) ) )
            return 0;
    }
    return 1;
}


int rotate_log_files( const char *src_fname, int level )
{
    int size;
    char fname[20];

    _join_log_fname(fname, level+1);

    if( mcush_size( fname, &size ) )
    {
        if( (level+1) >= (LOGGER_ROTATE_LEVEL) )
        {
            mcush_remove( fname );
#if DEBUG_LOGGER
            shell_printf( "remove file %s\n", fname );
#endif
        }
        else
        {
            rotate_log_files( fname, level+1 );
        }
    }

#if DEBUG_LOGGER
    shell_printf( "rename file %s -> %s\n", src_fname, fname );
#endif
    return mcush_rename( src_fname, &fname[3] ); /* remove leading mount point */
}

void task_logger_entry(void *p)
{
    logger_event_t evt;
#if MCUSH_SPIFFS
    int fd = 0;
    char buf[256];
    int size = -1;
    int i, j;
#endif

    while( 1 )
    {
        hal_wdg_clear();
        if( xQueueReceive( queue_logger, &evt, portMAX_DELAY ) != pdTRUE )
            continue;
        
#if MCUSH_SPIFFS
        /* check file size from filesystem only once */ 
        if( size < 0 )
        {
            if( mcush_size( _fname, &size ) == 0 )
                size = 0;  /* read error, skip */
        }

        /* log files rotate if needed */ 
        if( size > LOGGER_FSIZE_LIMIT )
        {
            hal_wdg_clear();
            rotate_log_files( _fname, 0 );
            size = -1;
        }

        /* try to create/append logfile */
        if( _enable )
        {
            hal_wdg_clear();
            fd = mcush_open( _fname, "a+" );
            if( fd != 0 )
            {
                convert_logger_event_to_str( &evt, buf );
                i = strlen(buf);
                j = mcush_write( fd, buf, i );
                size = size < 0 ? j : size + j;
                mcush_close( fd );
                fd = 0;
                if( i != j )
                    set_errno( ERRNO_FILE_READ_WRITE_ERROR );
            }
            else
                set_errno( ERRNO_FILE_READ_WRITE_ERROR );
        }
#endif
        /* forward event to shell_monitor or clean up directly */ 
        if( monitoring_mode )
        {
            /* forward the event */
            if( xQueueSend( queue_logger_monitor, &evt, 0 ) != pdTRUE )
            {
                free(evt.str);
            }
        }
        else
        {
            free(evt.str);
        }
    }
}



int cmd_logger( int argc, char *argv[] );
const shell_cmd_t cmd_tab_logger[] = {
{   0, 0, "log",  cmd_logger, 
    "disp logger",
    "logger -d|u"  },
{   CMD_END  } };


void task_logger_init(void)
{
    TaskHandle_t task_logger;

    queue_logger = xQueueCreate(TASK_LOGGER_QUEUE_SIZE, (unsigned portBASE_TYPE)sizeof(logger_event_t));  
    if( queue_logger == NULL )
        halt("create logger queue");
    vQueueAddToRegistry( queue_logger, "logQ" );

    queue_logger_monitor = xQueueCreate(TASK_LOGGER_MONITOR_QUEUE_SIZE, (unsigned portBASE_TYPE)sizeof(logger_event_t));  
    if( queue_logger_monitor == NULL )
        halt("create logger monitor queue");
    vQueueAddToRegistry( queue_logger_monitor, "logMQ" );

    xTaskCreate(task_logger_entry, (const char *)"logT", 
        TASK_LOGGER_STACK_SIZE / sizeof(portSTACK_TYPE),
        NULL, TASK_LOGGER_PRIORITY, &task_logger);
    if( task_logger == NULL )
        halt("create logger task");
    mcushTaskAddToRegistered((void*)task_logger);
    
    shell_add_cmd_table( cmd_tab_logger );
}


int cmd_logger( int argc, char *argv[] )
{
    static const mcush_opt_spec opt_spec[] = {
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'd', shell_str_disable, 0, "disable logging to file" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'e', shell_str_enable, 0, "enable logging to file" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          0, shell_str_delete, 0, "delete history files" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'h', shell_str_history, 0, "list history from log file" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'D', shell_str_debug, 0, "DEBUG type filter" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'I', shell_str_info, 0, "INFO type filter" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'W', shell_str_warn, 0, "WARN type filter" },
        { MCUSH_OPT_SWITCH, MCUSH_OPT_USAGE_REQUIRED, 
          'E', shell_str_error, 0, "ERROR type filter" },
        { MCUSH_OPT_VALUE, MCUSH_OPT_USAGE_REQUIRED | MCUSH_OPT_USAGE_VALUE_REQUIRED, 
          'H', shell_str_head, "head", "message head filter" },
        { MCUSH_OPT_VALUE, MCUSH_OPT_USAGE_REQUIRED | MCUSH_OPT_USAGE_VALUE_REQUIRED, 
          'm', shell_str_msg, shell_str_message, "log message" },
        { MCUSH_OPT_NONE } };
    mcush_opt_parser parser;
    mcush_opt opt;
    int8_t enable, enable_set=0, history_set=0, debug_set=0, info_set=0, warn_set=0, error_set=0, delete_set=0;
    int8_t filter_mode=0;
    const char *msg=0, *head=0;
    uint8_t head_len=0;
    logger_event_t evt;
    char c;
    char buf[256];

    mcush_opt_parser_init(&parser, opt_spec, (const char **)(argv+1), argc-1 );
    while( mcush_opt_parser_next( &opt, &parser ) )
    {
        if( opt.spec )
        {
            if( STRCMP( opt.spec->name, shell_str_enable ) == 0 )
            {
                enable = 1;
                enable_set = 1;
            }
            else if( STRCMP( opt.spec->name, shell_str_disable ) == 0 )
            {
                enable = 0;
                enable_set = 1;
            }
            else if( STRCMP( opt.spec->name, shell_str_history ) == 0 )
                history_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_delete ) == 0 )
                delete_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_debug ) == 0 )
                debug_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_info ) == 0 )
                info_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_warn ) == 0 )
                warn_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_error ) == 0 )
                error_set = 1;
            else if( STRCMP( opt.spec->name, shell_str_head ) == 0 )
            {
                head = opt.value;
                if( head == 0 )
                {
                    shell_write_err(shell_str_head);
                    return -1;
                }
                else
                    head_len = strlen(head);
            }
            else if( STRCMP( opt.spec->name, shell_str_msg ) == 0 )
            {
                msg = opt.value;
                if( msg == 0 )
                {
                    shell_write_err(shell_str_msg);
                    return -1;
                }
            }
 
        }
        else
            STOP_AT_INVALID_ARGUMENT 
    }

    if( enable_set )
    {
        _enable = enable;
        return 0;
    }
  
    if( delete_set )
    {
        hal_wdg_clear();
        return delete_all_log_files() ? 0 : 1;
    }
 
    /* priority: 
       1 - add new message 
       2 - view history
       3 - monitor
     */
    if( msg )
    {
        logger_str( LOG_INFO, msg );
    }
    else if( history_set )
    {
        /* TODO: similar to tail command
           read file line by line, record last 10 lines, print them all when EOF */
    }
    else
    {
        monitoring_mode = 1;
        if( debug_set )
            filter_mode |= LOG_DEBUG;
        if( info_set )
            filter_mode |= LOG_INFO;
        if( warn_set )
            filter_mode |= LOG_WARN;
        if( error_set )
            filter_mode |= LOG_ERROR;
        if( filter_mode == 0 )
            filter_mode = LOG_DEBUG | LOG_INFO | LOG_WARN | LOG_ERROR;  /* all messages allowed */
        while( 1 )
        {
            if( xQueueReceive( queue_logger_monitor, &evt, 100*configTICK_RATE_HZ/1000 ) == pdTRUE )
            {
                if( evt.type & filter_mode )
                {
                    if( (head==0) || (strncmp(evt.str, head, head_len) == 0) )
                    {
                        convert_logger_event_to_str( &evt, buf );
                        shell_write_str( buf );
                    }
                }
                free( evt.str );
            }

            do
            {
                c = 0;
                if( shell_driver_read_char_blocked(&c, 0) == -1 )
                    break;
                if( c == 0x03 ) /* Ctrl-C for stop */
                    break;
            } while(1);
            if( c == 0x03 )
                break;
        }  
        monitoring_mode = 0;
        /* free all remaining event */
        while( xQueueReceive( queue_logger_monitor, &evt, 0 ) == pdTRUE ) 
        {
            free( evt.str );
        }
    } 
    return 0;
}


