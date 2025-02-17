/*
 * pacsat_command.c
 *
 *  Created on: May 31, 2024
 *      Author: g0kla
 */

/* System include files */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

/* Program Include Files */
#include "config.h"
#include "iors_command.h"
#include "state_file.h"
#include "debug.h"
#include "pacsat_header.h"
#include "pacsat_broadcast.h"
#include "pacsat_command.h"
#include "pacsat_dir.h"
#include "str_util.h"
#include "ax25_tools.h"
#include "iors_log.h"

/* Static vars*/
static int last_command_rc = EXIT_SUCCESS;;

int pc_execute_file_in_folder(DIR_NODE *node, char *folder, uint16_t exec_arg1, uint16_t exec_arg2);
int pc_delete_file_from_folder(DIR_NODE *node, char *folder, int is_directory_folder);

/**
 * pb_handle_command()
 *
 * Send a received command to the command task.
 *
 * Returns EXIT_SUCCESS if it could be processed, otherwise it returns EXIT_FAILURE
 *
 */
int pc_handle_command(char *from_callsign, unsigned char *data, int len) {
		struct t_ax25_header *ax25_header;
		ax25_header = (struct t_ax25_header *)data;
		if ((ax25_header->pid & 0xff) != PID_COMMAND) {
			return EXIT_FAILURE;
		}
		SWCmdUplink *sw_command;
		sw_command = (SWCmdUplink *)(data + sizeof(AX25_HEADER));

		if(sw_command->namespaceNumber != SWCmdNSPacsat) return EXIT_SUCCESS; // This was not for us, ignore

//		debug_print("Received PACSAT Command %04x addr: %d names: %d cmd %d from %s length %d\n",(sw_command->dateTime),
//				sw_command->address, sw_command->namespaceNumber, (sw_command->comArg.command), from_callsign, len);

		//	int i;
	//	for (i=0; i<4; i++)
	//		debug_print("arg:%d %d\n",i,sw_command->comArg.arguments[i]);
		/* Pass the data to the command processor */
		int cmd_rc = AuthenticateSoftwareCommand(sw_command);
		if (cmd_rc == EXIT_FAILURE){
			int r = pb_send_err(from_callsign, PB_ERR_FILE_INVALID_PACKET);
			if (r != EXIT_SUCCESS) {
				debug_print("\n Error : Could not send ERR Response to TNC \n");
			}
			last_command_rc = EXIT_FAILURE;
			return EXIT_FAILURE;
		}
		int rc;
		if (cmd_rc == EXIT_DUPLICATE) {
			if (last_command_rc == EXIT_SUCCESS)
			    rc = pb_send_ok(from_callsign);
			else
				rc = pb_send_err(from_callsign, last_command_rc);
			if (rc != EXIT_SUCCESS) {
				debug_print("\n Error : Could not send OK Response to TNC \n");
			}
			return EXIT_SUCCESS; // Duplicate
		}
		last_command_rc = EXIT_SUCCESS; // We can also change this if we in fact send an err or ok command

//		debug_print("Auth Command\n");
		log_alog2f(WARN_LOG, g_log_filename, ALOG_COMMAND, from_callsign, 0, sw_command->namespaceNumber, sw_command->comArg.command,
				sw_command->comArg.arguments[0],sw_command->comArg.arguments[1],sw_command->comArg.arguments[2],sw_command->comArg.arguments[3]);

		switch (sw_command->comArg.command) {
			case SWCmdPacsatEnablePB: {
				//debug_print("Enable PB Command\n");
				g_state_pb_open = sw_command->comArg.arguments[0];
				if (sw_command->comArg.arguments[1]) {
					g_pb_status_period_in_seconds = sw_command->comArg.arguments[1];
				}
				if (sw_command->comArg.arguments[2]) {
					g_pb_max_period_for_client_in_seconds = sw_command->comArg.arguments[2];
				}
				last_command_rc = EXIT_SUCCESS;
				int rc = pb_send_ok(from_callsign);
				if (rc != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send OK Response to TNC \n");
				}
				save_state();
				break;
			}
			case SWCmdPacsatEnableUplink: {
				//debug_print("Enable Uplink Command\n");
				g_state_uplink_open = sw_command->comArg.arguments[0];
				if (sw_command->comArg.arguments[1]) {
					g_uplink_status_period_in_seconds = sw_command->comArg.arguments[1];
				}
				if (sw_command->comArg.arguments[2]) {
					g_uplink_max_period_for_client_in_seconds = sw_command->comArg.arguments[2];
				}
				last_command_rc = EXIT_SUCCESS;
				int rc = pb_send_ok(from_callsign);
				if (rc != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send OK Response to TNC \n");
				}
				save_state();
				break;
			}
			case SWCmdPacsatInstallFile: {
				/* Args are 32 bit fild id, 16 bit folder id */
				uint32_t file_id = sw_command->comArg.arguments[0] + (sw_command->comArg.arguments[1] << 16) ;
				uint16_t folder_id = sw_command->comArg.arguments[2];
				//dir_debug_print(NULL);

				if (folder_id == FolderDir) {
					debug_print("Error - cant install into Directory\n");
					last_command_rc = PB_ERR_FILE_INVALID_PACKET;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_INVALID_PACKET);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}

				DIR_NODE *node = dir_get_node_by_id(file_id);
				if (node == NULL) {
					error_print("File %d not available\n",file_id);
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}
				//debug_print("Installing %d into %s with keywords %s\n",node->pfh->fileId, node->pfh->userFileName, node->pfh->keyWords);

				char *folder = get_folder_str(folder_id);
				if (folder == NULL) {
					//debug_print("Error - invalid folder\n");
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}

				//debug_print("Install File: %04x : %s into dir: %d - %s | File Name:%d\n",*arg0, source_file, *arg1, dest_file, *arg2);
				if (pfh_extract_file_and_update_keywords(node->pfh, folder, true) != EXIT_SUCCESS) {
					debug_print("Error extracting file into %s\n",folder);
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}
				last_command_rc = EXIT_SUCCESS;
				int rc = pb_send_ok(from_callsign);
				if (rc != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send OK Response to TNC \n");
				}

				/* We updated the PACSAT dir. Reload. */
				dir_load();

				//dir_debug_print(NULL);

				break;
			}

			case SwCmdPacsatExecuteFile: {
				uint32_t file_id = sw_command->comArg.arguments[0] + (sw_command->comArg.arguments[1] << 16) ;
				uint16_t exec_arg1 = sw_command->comArg.arguments[2];
				uint16_t exec_arg2 = sw_command->comArg.arguments[3];

				char *folder = get_folder_str(FolderBin);
				if (folder == NULL) {
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					break;
				}

				DIR_NODE *node = dir_get_node_by_id(file_id);
				if (node == NULL) {
					error_print("File %ld not available\n",file_id);
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}

				int rc = pc_execute_file_in_folder(node, folder, exec_arg1, exec_arg2);
				if (rc == EXIT_SUCCESS) {
					last_command_rc = EXIT_SUCCESS;
					int rc = pb_send_ok(from_callsign);
					if (rc != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send OK Response to TNC \n");
					}
				} else {
					last_command_rc = PB_ERR_FILE_INVALID_PACKET;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_INVALID_PACKET);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
				}

				break;
			}

			case SWCmdPacsatDeleteFile: {
//				debug_print("Arg: %02x %02x\n",sw_command->comArg.arguments[0],sw_command->comArg.arguments[1]);
				uint32_t file_id = sw_command->comArg.arguments[0] + (sw_command->comArg.arguments[1] << 16) ;
				uint16_t folder_id = sw_command->comArg.arguments[2];

				char *folder = get_folder_str(folder_id);
				if (folder == NULL) {
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					break;
				}
				int is_directory_folder = false;
				if (folder_id == FolderDir)
					is_directory_folder = true;

				DIR_NODE *node = dir_get_node_by_id(file_id);
				if (node == NULL) {
					error_print("File %ld not available\n",file_id);
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}

				int rc = pc_delete_file_from_folder(node, folder, is_directory_folder);
				if (rc == EXIT_SUCCESS) {
					last_command_rc = EXIT_SUCCESS;
					int rc = pb_send_ok(from_callsign);
					if (rc != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send OK Response to TNC \n");
					}
					node->pfh->uploadTime = 0; /* This will be allocated the current time when we reload the dir below */
					if (pfh_update_pacsat_header(node->pfh, get_dir_folder()) != EXIT_SUCCESS) {
						debug_print("** Failed to re-write header in file.\n");
					}

					/* We updated the PACSAT dir. Reload. */
					dir_load();
				} else {
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
				}

				break;
			}
			case SWCmdPacsatDeleteFolder: {
				uint16_t folder_id = sw_command->comArg.arguments[0];
				int purge_orphan_files = sw_command->comArg.arguments[1];

				char *folder = get_folder_str(folder_id);
				if (folder == NULL) {
					last_command_rc = PB_ERR_TEMPORARY;
					int r = pb_send_err(from_callsign, PB_ERR_TEMPORARY);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}

				/* Send Ok here as command is valid and any other errors below are ignored. */
				last_command_rc = EXIT_SUCCESS;
				int rc = pb_send_ok(from_callsign);
				if (rc != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send OK Response to TNC \n");
				}

				int is_directory_folder = false;
				if (folder_id == FolderDir)
					is_directory_folder = true;

				DIR_NODE *node;
				DIR_NODE *next_node = NULL;

				while(node != NULL) {
					node = dir_get_pfh_by_folder_id(folder, next_node );
					if (node != NULL) {
						/* We have a header installed in this folder */
						//debug_print("Removing: File id %d from folder %s\n", node->pfh->fileId, folder);
						pc_delete_file_from_folder(node, folder, is_directory_folder);
						node->pfh->uploadTime = 0; /* These will all be allocated upload times when we reload below */
						if (pfh_update_pacsat_header(node->pfh, get_dir_folder()) != EXIT_SUCCESS) {
							debug_print("** Failed to re-write header in file.\n");
						}
						if (node->next == NULL) {
							break; // we are at end of dir
						} else {
							next_node = node->next;
						}
					}
				}

				// Purge all other files
				if (purge_orphan_files) {
					char dir_folder[MAX_FILE_PATH_LEN];
					strlcpy(dir_folder, get_data_folder(), MAX_FILE_PATH_LEN);
					strlcat(dir_folder, "/", MAX_FILE_PATH_LEN);
					strlcat(dir_folder, folder, MAX_FILE_PATH_LEN);
					//debug_print("Purging remaining files from: %s\n",dir_folder);
					DIR * d = opendir(dir_folder);
					if (d == NULL) {
						error_print("** Could not open dir: %s\n",dir_folder);
					} else {
						struct dirent *de;
						for (de = readdir(d); de != NULL; de = readdir(d)) {
							char orphan_file_name[MAX_FILE_PATH_LEN];
							strlcpy(orphan_file_name, dir_folder, sizeof(orphan_file_name));
							strlcat(orphan_file_name, "/", sizeof(orphan_file_name));
							strlcat(orphan_file_name, de->d_name, sizeof(orphan_file_name));
							if ((strcmp(de->d_name, ".") != 0) && (strcmp(de->d_name, "..") != 0)) {
								//debug_print("Purging: %s\n",orphan_file_name);
								remove(orphan_file_name);
							}
						}
						closedir(d);
					}
				}

				/* We update the PACSAT dir. Reload. */
				dir_load();
				break;
			}

			case SWCmdPacsatDefaultFileExpiryPeriod: {
				uint16_t age = sw_command->comArg.arguments[0] ;
				g_dir_max_file_age_in_seconds = age * 24 * 60 * 60;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				int rc = pb_send_ok(from_callsign);
				if (rc != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send OK Response to TNC \n");
				}
				break;
			}
			case SWCmdPacsatFileExpiryPeriod: {
				uint32_t file_id = sw_command->comArg.arguments[0] + (sw_command->comArg.arguments[1] << 16);
				uint32_t file_age = sw_command->comArg.arguments[2] + (sw_command->comArg.arguments[3] << 16);
				//This needs to set the expiry time on a specific file
				DIR_NODE *node = dir_get_node_by_id(file_id);
				if (node == NULL) {
					error_print("File %ld not available\n",file_id);
					last_command_rc = PB_ERR_FILE_NOT_AVAILABLE;
					int r = pb_send_err(from_callsign, PB_ERR_FILE_NOT_AVAILABLE);
					if (r != EXIT_SUCCESS) {
						debug_print("\n Error : Could not send ERR Response to TNC \n");
					}
					break;
				}
				/* Set the expire date on that file. */
				node->pfh->expireTime = file_age;
				if (pfh_update_pacsat_header(node->pfh, get_dir_folder()) != EXIT_SUCCESS) {
					debug_print("** Failed to re-write header in file.\n");
				}
				break;
			}
			case SWCmdPacsatDirMaintPeriod: {
				uint16_t dir_period = sw_command->comArg.arguments[0];
				g_dir_maintenance_period_in_seconds = dir_period;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				pb_send_ok(from_callsign);
				break;
			}
			case SWCmdPacsatFtl0MaintPeriod: {
				uint16_t ftl0_period = sw_command->comArg.arguments[0];
				g_ftl0_maintenance_period_in_seconds = ftl0_period;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				pb_send_ok(from_callsign);
				break;
			}
			case SWCmdPacsatFileQueueCheckPeriod: {
				uint16_t check_period = sw_command->comArg.arguments[0];
				g_file_queue_check_period_in_seconds = check_period;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				pb_send_ok(from_callsign);
				break;
			}
			case SWCmdPacsatMaxFileSize: {
				uint16_t file_size = sw_command->comArg.arguments[0];
				g_ftl0_max_file_size = file_size * 1024;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				pb_send_ok(from_callsign);
				break;
			}
			case SWCmdPacsatMaxUploadAge: {
				uint16_t upload_file_age = sw_command->comArg.arguments[0];
				g_ftl0_max_upload_age_in_seconds = upload_file_age * 24 * 60 * 60;
				save_state();

				last_command_rc = EXIT_SUCCESS;
				pb_send_ok(from_callsign);
				break;
			}
			default:
				error_print("\n Error : Unknown pacsat command: %d\n",sw_command->comArg.command);
				last_command_rc = PB_ERR_COMMAND_NOT_AVAILABLE;
				int r = pb_send_err(from_callsign, PB_ERR_COMMAND_NOT_AVAILABLE);
				if (r != EXIT_SUCCESS) {
					debug_print("\n Error : Could not send ERR Response to TNC \n");
				}

				return EXIT_FAILURE;
				break;
		}
		return EXIT_SUCCESS;
}

int pc_execute_file_in_folder(DIR_NODE *node, char *folder, uint16_t exec_arg1, uint16_t exec_arg2) {
	char dest_file[MAX_FILE_PATH_LEN];
		//char file_name[10];
		//snprintf(file_name, 10, "%04x",node->pfh->fileId);
		strlcpy(dest_file, get_data_folder(), MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, folder, MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, node->pfh->userFileName, MAX_FILE_PATH_LEN);

		struct stat st = {0};
		if (stat(dest_file, &st) == -1) {
			// No file exists, cant execute
			return EXIT_FAILURE;
		}
//		debug_print("%s : RWX:%d %d %d\n",dest_file,st.st_mode & S_IRUSR,st.st_mode & S_IWUSR,st.st_mode & S_IXUSR);
		if ((st.st_mode & S_IXUSR) != S_IXUSR) {
			// make executable
//			debug_print("Setting Execute Bit\n");
			if (chmod(dest_file, st.st_mode | S_IXUSR) != EXIT_SUCCESS) {
				return EXIT_FAILURE;
			}
		}

		char args[25];
		snprintf(args, 25, " %d %d",exec_arg1, exec_arg2);
		strlcat(dest_file, args, MAX_FILE_PATH_LEN);

		debug_print("Execute File by userfilename: %04x in dir: %s\n",node->pfh->fileId, dest_file);
		int cmd_rc = system(dest_file);

		if (cmd_rc == EXIT_SUCCESS) {
			return EXIT_SUCCESS;
		} else {
			return EXIT_FAILURE;
		}

}

int pc_delete_file_from_folder(DIR_NODE *node, char *folder, int is_directory_folder) {
//	debug_print("Deleting %d from %s with keywords %s\n",node->pfh->fileId, node->pfh->userFileName, node->pfh->keyWords);
	char dest_file[MAX_FILE_PATH_LEN];
	char file_name[10];
	snprintf(file_name, 10, "%04x",node->pfh->fileId);
	if (is_directory_folder || strlen(node->pfh->userFileName) == 0) {
		strlcpy(dest_file, get_data_folder(), MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, folder, MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, file_name, MAX_FILE_PATH_LEN);
		if (is_directory_folder) {
			strlcat(dest_file, ".", MAX_FILE_PATH_LEN);
			strlcat(dest_file, PSF_FILE_EXT, MAX_FILE_PATH_LEN);
		}
	} else {
		strlcpy(dest_file, get_data_folder(), MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, folder, MAX_FILE_PATH_LEN);
		strlcat(dest_file, "/", MAX_FILE_PATH_LEN);
		strlcat(dest_file, node->pfh->userFileName, MAX_FILE_PATH_LEN);
		//debug_print("Delete File by userfilename: %04x in dir: %s - %s\n",node->pfh->fileId, folder, dest_file);
	}
//	debug_print("Remove: %s\n",dest_file);
	struct stat st = {0};
	if (stat(dest_file, &st) == -1) {
		// No file exists, but try to remove the redundant keywords
		pfh_remove_keyword(node->pfh, folder);
		return EXIT_SUCCESS;
	}

	if (remove(dest_file) == EXIT_SUCCESS) {
		/* If successful we change the header to remove the keyword for the installed dir and set the upload date */
		pfh_remove_keyword(node->pfh, folder);
		return EXIT_SUCCESS;
	} else {
		return EXIT_FAILURE;
	}

}
