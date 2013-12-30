
#ifndef __CONFIG_H__
#define __CONFIG_H__

#include <sys/types.h>
#include <limits.h>

#define MAX_IP_LEN	16

typedef struct config_t
{
	char 		ip[MAX_IP_LEN];	
	u_int16_t	port;
	char 		service_ip[MAX_IP_LEN];
	char		etc_path[PATH_MAX];
	char		bin_path[PATH_MAX];
	char		log_path[PATH_MAX];
	char		html_path[PATH_MAX];
	//char		work_path[PATH_MAX];
	char		channels_file[PATH_MAX];
	int32_t		max_clip_num;
	int32_t		download_interval;
	//bps
	u_int64_t	download_limit;
} CONFIG_T;

extern CONFIG_T	g_config;

int config_read(CONFIG_T* configp, char* file_name);

#endif
