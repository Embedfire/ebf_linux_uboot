#include <common.h>
#include <command.h>
#include <linux/ctype.h>
#include <linux/types.h>
#include <asm/global_data.h>
#include <linux/libfdt.h>
#include <fdt_support.h>
#include <mapmem.h>
#include <asm/io.h>
#include <linux/string.h>
#include <fdt_support.h>
#include <malloc.h>
#include <dtoverlay.h>
#include <fs.h>

static int do_dtfile(cmd_tbl_t *cmdtp, int flag, int argc, char * const argv[])
{
	char *data, *sp, *dp, *name, *value = NULL,*ptr,*env_file, *buf;
	ulong file_addr,dt_addr,overlay_addr;
	loff_t	size;
	int param_num = 0;
	int dtbo_num = 0;
	char * dtparam_name[30],*dt_s;
	char * dtparam_value[30];


	dtoverlay_debug("param no:%d  %s  %s %s %s \n",argc,argv[1],argv[2],argv[3],argv[4]);

	if (argc < 5)
		return CMD_RET_USAGE;

	dt_addr = simple_strtoul(argv[1], NULL, 16);

	overlay_addr = simple_strtoul(argv[2], NULL, 16);

	env_file = argv[3];

	file_addr = simple_strtoul(argv[4], NULL, 16);

//	load_env_file(env_file,file_addr,&size);
    buf = env_get("size");
	size = simple_strtoul(buf, NULL, 16);

	ptr = map_sysmem(file_addr, 0);

	if ((data = malloc(size + 1)) == NULL) {
		dtoverlay_debug("himport_r: can't malloc %lu bytes\n", (ulong)size + 1);
		__set_errno(ENOMEM);
		return 0;
	}

	memcpy(data, ptr, size);
	data[size] = '\0';
	dp = data;

	fdt_shrink_to_minimum((void *)dt_addr, 0x30000);

	do
	{
		if (strncmp(dp, "#overlay_start",14) != 0) {
			while (*dp && (*dp != '\n'))
				++dp;
			++dp;
			continue;
		}
		else
		{
			break;
		}
	} while ((dp < data + size) && *dp);
	
	do {

		/* skip leading white space */
		while (isblank(*dp))
			++dp;
		/* skip comment lines */
		

		if (*dp == '#') {
			while (*dp && (*dp != '\n'))
				++dp;
			++dp;
			continue;
		}
		/* parse name */
		for (name = dp; *dp != '=' && *dp && *dp != '\n'; ++dp);
			/* deal with "name" and "name=" entries (delete var) */
		if (*dp == '\0' || *(dp + 1) == '\0' ||
			*dp == '\n' || *(dp + 1) == '\n') {
			if (*dp == '=')
				*dp++ = '\0';
			*dp++ = '\0';	/* terminate name */

			continue;
		}
		*dp++ = '\0';	/* terminate name */

		/* parse value; deal with escapes */
		for (value = sp = dp; *dp && (*dp != '\n'); ++dp) {
			if ((*dp == '\\') && *(dp + 1))
				++dp;
			*sp++ = *dp;
		}
		*sp++ = '\0';	/* terminate value */
		++dp;
		if(strncmp(name, "dtparam",7) == 0)
			{
				for(dt_s = value;*dt_s && *dt_s!='\n';++dt_s)
				{
					  if ((*dt_s == '=') && *(dt_s + 1))
						{
							*dt_s = '\0';
							dtparam_name[param_num] = value;
							dtparam_value[param_num] = dt_s + 1;

						}
				}
				if(NULL == dtparam_value)
				{
					dtparam_value[param_num] = "true";
				}
				param_num++;
				*dt_s = '\0';
			}
		else if(strncmp(name, "dtoverlay",9) == 0)
			{
				char *overlay_name = NULL,*ps;
				char *overlay_para_name[30] = {};
				char *overlay_para_value[30] = {};

				for(dtbo_num = 0,ps = value;*ps && *ps!='\n';++ps)
				{
					if ((*ps == ',') && *(ps + 1))
						{
							
							if(dtbo_num == 0)
							{
								 overlay_name = value;
							}
							 *ps = '\0';
							overlay_para_name[dtbo_num]= ps+1;
						}
					else if ((*ps == '=') && *(ps + 1))
						{
							*ps = '\0';
							overlay_para_value[dtbo_num] = ps+1;
							dtbo_num++;

							if(*(ps++) == '\n')
							{
								 *ps = '\0';
								 break;
							}                  
						}    
				}
				if(NULL == overlay_name)
					overlay_name = value;
				if(value != NULL)
				{
					edit_dtparam(overlay_addr,overlay_name,overlay_para_name,overlay_para_value,dtbo_num);
					fdt_overlay_apply_verbose((void *)dt_addr, (void *)overlay_addr);
				}
			}
		if (strncmp(dp, "#overlay_end",12) == 0) {
			break;
			}
		} while ((dp < data + size) && *dp);

	if(param_num != 0)
	{
		edit_dtparam(dt_addr," ",dtparam_name,dtparam_value,param_num);
	}
	free(data);
	return 0;

}

static char dtfile_help_text[] =
	"dtfile <dt addr> <overlays addr> <config name> <config addr>  - load the uEnv.txt to <fileaddr>\n";

U_BOOT_CMD(
	dtfile,	255,	0,	do_dtfile,
	"dtoverlay utility commands", dtfile_help_text
);