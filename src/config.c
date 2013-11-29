/*This code is part of the Epoch Init System.
* The Epoch Init System is maintained by Subsentient.
* This software is public domain.
* Please read the file UNLICENSE.TXT for more information.*/

/**This file handles the parsing of epoch.conf, our configuration file.
 * It adds everything into the object table.**/
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <signal.h>
#include <ctype.h>
#include "epoch.h"

/*We want the only interface for this to be LookupObjectInTable().*/
ObjTable *ObjectTable = NULL;

/*Used to allow for things like 'ObjectStartPriority Services', where Services == 3, for example.*/
static struct _PriorityAliasTree
{ /*Start/Stop priority alias support for grouping.*/
	char Alias[MAX_DESCRIPT_SIZE];
	unsigned long Target;
	
	struct _PriorityAliasTree *Next;
	struct _PriorityAliasTree *Prev;
} *PriorityAliasTree = NULL;

/*Used to allow runlevels to be inherited by other runlevels.*/
static struct _RunlevelInheritance
{ /*I __REVILE__ this solution.
Epoch is just a linked list of linked lists anymore.*/
	char Inheriter[MAX_DESCRIPT_SIZE];
	char Inherited[MAX_DESCRIPT_SIZE];
	
	struct _RunlevelInheritance *Next;
	struct _RunlevelInheritance *Prev;
} *RunlevelInheritance = NULL;

/*Holds the system hostname.*/
char Hostname[MAX_LINE_SIZE] = { '\0' };

/*Function forward declarations for all the statics.*/
static ObjTable *AddObjectToTable(const char *ObjectID);
static char *NextLine(const char *InStream);
static rStatus GetLineDelim(const char *InStream, char *OutStream);
static rStatus ScanConfigIntegrity(void);
static void ConfigProblem(short Type, const char *Attribute, const char *AttribVal, unsigned long LineNum);
static unsigned long PriorityAlias_Lookup(const char *Alias);
static void PriorityAlias_Add(const char *Alias, unsigned long Target);
static void PriorityAlias_Shutdown(void);
static void RLInheritance_Add(const char *Inheriter, const char *Inherited);
static Bool RLInheritance_Check(const char *Inheriter, const char *Inherited);
static void RLInheritance_Shutdown(void);

/*Used for error handling in InitConfig() by ConfigProblem().*/
enum { CONFIG_EMISSINGVAL = 1, CONFIG_EBADVAL, CONFIG_ETRUNCATED, CONFIG_EAFTER, CONFIG_EBEFORE, CONFIG_ELARGENUM };

/*Actual functions.*/
static char *NextLine(const char *InStream)
{
	if (!(InStream = strstr(InStream, "\n")))
	{
		return NULL;
	}

	if (*(InStream + 1) == '\0')
	{ /*End of file.*/
		return NULL;
	}

	++InStream; /*Plus one for the newline. We want to skip past it.*/

	return (char*)InStream;
}

/*This function was so useful I gave it external linkage.*/
char *WhitespaceArg(const char *InStream)
{  /*This is used for parsing lines that need to be divided by spaces.*/
	while (*InStream != ' ' && *InStream != '\t' &&
			*InStream != '\n' && *InStream != '\0') ++InStream;
	
	if (*InStream == '\n' || *InStream == '\0')
	{
		return NULL;
	}
	
	while (*InStream == ' ' || *InStream == '\t') ++InStream;
	
	if (*InStream == '\0' || *InStream == '\n')
	{
		return NULL;
	}
	
	return (char*)InStream;
}


static void ConfigProblem(short Type, const char *Attribute, const char *AttribVal, unsigned long LineNum)
{ /*Special little error handler used by InitConfig() to prevent repetitive duplicate errors.*/
	char TmpBuf[1024];
	char LogBuffer[MAX_LINE_SIZE];

	switch (Type)
	{
		case CONFIG_EMISSINGVAL:
			snprintf(TmpBuf, 1024, "Missing or bad value for attribute %s in epoch.conf line %lu.\nIgnoring.",
					Attribute, LineNum);
			break;
		case CONFIG_EBADVAL:
			snprintf(TmpBuf, 1024, "Bad value %s for attribute %s in epoch.conf line %lu.", AttribVal, Attribute, LineNum);
			break;
		case CONFIG_ETRUNCATED:
			snprintf(TmpBuf, 1024, "Attribute %s in epoch.conf line %lu has\n"
					"abnormally long value and may have been truncated.", Attribute, LineNum);
			break;
		case CONFIG_EAFTER:
			snprintf(TmpBuf, 1024, "Attribute %s cannot be set after an ObjectID attribute; "
					"epoch.conf line %lu. Ignoring.", Attribute, LineNum);
			break;
		case CONFIG_EBEFORE:
			snprintf(TmpBuf, 1024, "Attribute %s comes before any ObjectID attribute.\n"
					"epoch.conf line %lu. Ignoring.", Attribute, LineNum);
			break;
		case CONFIG_ELARGENUM:
			snprintf(TmpBuf, 1024, "Attribute %s in epoch.conf line %lu has\n"
					"abnormally high numeric value and may cause malfunctions.", Attribute, LineNum);
			break;
		default:
			return;
	}
	
	snprintf(LogBuffer, MAX_LINE_SIZE, "CONFIG: " CONSOLE_COLOR_YELLOW "WARNING: " CONSOLE_ENDCOLOR "%s\n", TmpBuf);
	
	SpitWarning(TmpBuf);
	WriteLogLine(LogBuffer, true);
}

rStatus InitConfig(void)
{ /*Set aside storage for the table.*/
	FILE *Descriptor = NULL;
	struct stat FileStat;
	char *ConfigStream = NULL, *Worker = NULL;
	ObjTable *CurObj = NULL, *ObjWorker = NULL;
	char DelimCurr[MAX_LINE_SIZE] = { '\0' };
	unsigned long LineNum = 1;
	const char *CurrentAttribute = NULL;
	Bool LongComment = false;
	char ErrBuf[MAX_LINE_SIZE];
	
	/*Get the file size of the config file.*/
	if (stat(CONFIGDIR CONF_NAME, &FileStat) != 0)
	{ /*Failure?*/
		SpitError("Failed to obtain information about configuration file epoch.conf.\nDoes it exist?");
		return FAILURE;
	}
	else
	{ /*No? Use the file size to allocate space in memory, since a char is a byte big.
	* If it's not a byte on your platform, your OS is not UNIX, and Epoch was not designed for you.*/
		ConfigStream = malloc(FileStat.st_size + 1);
	}

	Descriptor = fopen(CONFIGDIR CONF_NAME, "r"); /*Open the configuration file.*/

	/*Read the file into memory. I don't really trust fread(), but oh well.
	 * People will whine if I use a loop instead.*/
	fread(ConfigStream, 1, FileStat.st_size, Descriptor);
	
	ConfigStream[FileStat.st_size] = '\0'; /*Null terminate.*/
	
	fclose(Descriptor); /*Close the file.*/

	Worker = ConfigStream;

	/*Empty file?*/
	if ((*Worker == '\n' && *(Worker + 1) == '\0') || *Worker == '\0')
	{
		SpitError("Seems that epoch.conf is empty or corrupted.");
		free(ConfigStream);
		return FAILURE;
	}

	do /*This loop does most of the parsing.*/
	{
		
		/*Allow whitespace to precede a line in case people want to create a block-styled appearance.*/
		while (*Worker == ' ' || *Worker == '\t') ++Worker;
		
		/**Multi-line comment support: Multi-line comments are created in the following way:
		 * >!> stuff
		 * stuff
		 * stuff stuff
		 * stuffy stuff
		 * <!< stuff
		 * stuff
		 * 
		 * It is not recognized to place a multi-line comment beginner or terminator anywhere but the beginning
		 * of the line. As such, one may place do things like "ObjectID >!>" to create an object with ID ">!>". **/

		if (!strncmp(Worker, "<!<", strlen("<!<")))
		{ /*It's probably not good to have stray multi-line comment terminators around.*/
			if (!LongComment)
			{
				snprintf(ErrBuf, MAX_LINE_SIZE, "Stray multi-line comment terminator on line %lu\n", LineNum);
				SpitWarning(ErrBuf);
				continue;
			}
			LongComment = false;
			
			/*Allow next line to begin right ater the terminator on the same line.*/
			Worker += strlen("<!<");
			while (*Worker == ' ' || *Worker == '\t') ++Worker;
		}
		else if (LongComment)
		{
			continue;
		}
		else if (!strncmp(Worker, ">!>", strlen(">!>")))
		{
			LongComment = true;
			continue;
		}
		
		/**Single-line comments are created by placing "#" at the beginning of the line. Placing them
		 * anywhere else has no effect and as such '#' may be used in commands and object IDs and descriptions.**/
		if (*Worker == '\n')
		{ /*Empty line.*/
			continue;
		}
		else if (*Worker == '#')
		{ /*Line is just a comment.*/
			continue;
		}
		
		/**Global configuration begins here.**/
		if (!strncmp(Worker, (CurrentAttribute = "DisableCAD"), strlen("DisableCAD")))
		{ /*Should we disable instant reboots on CTRL-ALT-DEL?*/
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}

			if (!strcmp(DelimCurr, "true"))
			{
				DisableCAD = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				DisableCAD = false;
			}
			else
			{				
				DisableCAD = true;
				
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}

			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "BlankLogOnBoot"), strlen("BlankLogOnBoot")))
		{ /*Should the log only hold the current boot cycle's logs?*/

			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}

			if (!strcmp(DelimCurr, "true"))
			{
				BlankLogOnBoot = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				BlankLogOnBoot = false;
			}
			else
			{				
				BlankLogOnBoot = false;
				
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}

			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ShellEnabled"), strlen("ShellEnabled")))
		{
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);

				continue;
			}
			
			if (!strcmp(DelimCurr, "true"))
			{
				ShellEnabled = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				ShellEnabled = false;
			}
			else
			{
				ShellEnabled = USE_SHELL_BY_DEFAULT;
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}
		}
		else if (!strncmp(Worker, (CurrentAttribute = "EnableLogging"), strlen("EnableLogging")))
		{
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);

				continue;
			}
			
			if (!strcmp(DelimCurr, "true"))
			{
				EnableLogging = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				EnableLogging = false;
			}
			else
			{
				
				EnableLogging = false;
				
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "RunlevelInherits"), strlen("RunlevelInherits")))
		{
			char Inheriter[MAX_DESCRIPT_SIZE], Inherited[MAX_DESCRIPT_SIZE];
			const char *TWorker = DelimCurr;
			unsigned long TInc = 0;
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EAFTER, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			for (; *TWorker != ' ' && *TWorker != '\t' && *TWorker != '\0' && TInc < MAX_DESCRIPT_SIZE - 1; ++TInc, ++TWorker)
			{
				Inheriter[TInc] = *TWorker;
			}
			Inheriter[TInc] = '\0';
			
			if (*TWorker == '\0')
			{
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
				continue;
			}
			
			TWorker = WhitespaceArg(TWorker);
			
			if (strstr(TWorker, " ") || strstr(TWorker, "\t"))
			{
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
				continue;
			}
			
			snprintf(Inherited, MAX_DESCRIPT_SIZE, "%s", TWorker);
			
			RLInheritance_Add(Inheriter, Inherited);
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "DefinePriority"), strlen("DefinePriority")))
		{
			char Alias[MAX_DESCRIPT_SIZE] = { '\0' };
			unsigned long Target = 0, TInc = 0;
			const char *TWorker = DelimCurr;
			
			if (CurObj != NULL)
			{ /*We can't allow this in object-local options, because then this may not be properly defined.*/
				ConfigProblem(CONFIG_EAFTER, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				
				continue;
			}
			
			for (; *TWorker != ' ' && *TWorker != '\t' &&
				*TWorker != '\0' && TInc < MAX_DESCRIPT_SIZE -1; ++TInc, ++TWorker)
			{ /*Copy in the identifier.*/
				Alias[TInc] = *TWorker;
			}
			Alias[TInc] = '\0';
			
			if (*TWorker == '\0')
			{
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
				continue;
			}
			
			TWorker = WhitespaceArg(TWorker); /*I abuse this delightful little function. It was meant for do-while loops.*/
			
			if (!AllNumeric(TWorker))
			{
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
				continue;
			}
			
			Target = atol(TWorker);
			
			PriorityAlias_Add(Alias, Target); /*Now add it to the linked list.*/
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "AlignStatusReports"), strlen("AlignStatusReports")))
		{
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);

				continue;
			}
			
			if (!strcmp(DelimCurr, "true"))
			{
				AlignStatusReports = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				AlignStatusReports = false;
			}
			else
			{
				
				AlignStatusReports = false;
				
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		/*This will mount /dev, /proc, /sys, /dev/pts, and /dev/shm on boot time, upon request.*/
		else if (!strncmp(Worker, (CurrentAttribute = "MountVirtual"), strlen("MountVirtual")))
		{
			const char *TWorker = DelimCurr;
			unsigned long Inc = 0;
			char CurArg[MAX_DESCRIPT_SIZE];
			const char *VirtualID[2][5] = { { "procfs", "sysfs", "devfs", "devpts", "devshm" },
											{ "procfs+", "sysfs+", "devfs+", "devpts+", "devshm+" } };
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);

				continue;
			}
			
			do
			{ /*Load in all the arguments in the line.*/
				Bool FoundSomething = false;
				
				for (Inc = 0; TWorker[Inc] != ' ' && TWorker[Inc] != '\t' && TWorker[Inc] != '\n' &&
					TWorker[Inc] != '\0' && Inc < (MAX_DESCRIPT_SIZE - 1); ++Inc)
				{ /*Copy in the argument for this line.*/
					CurArg[Inc] = TWorker[Inc];
				}
				CurArg[Inc] = '\0';
				
				
				for (Inc = 0; Inc < 5; ++Inc)
				{ /*Search through the argument to see what it matches.*/
					if (!strncmp(VirtualID[0][Inc], CurArg, strlen(VirtualID[0][Inc])))
					{
						AutoMountOpts[Inc] = (!strcmp(VirtualID[1][Inc], CurArg) ? 2 : true);
						FoundSomething = true;
						break;
					}
				}

				if (!FoundSomething)
				{ /*If it doesn't match anything, that's bad.*/
					ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
					
					continue;
				}
					
			} while ((TWorker = WhitespaceArg(TWorker)));
			
			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		/*Now we get into the actual attribute tags.*/
		else if (!strncmp(Worker, (CurrentAttribute = "BootBannerText"), strlen("BootBannerText")))
		{ /*The text shown at boot up as a kind of greeter, before we start executing objects. Can be disabled, off by default.*/
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!strcmp(DelimCurr, "NONE")) /*So, they decided to explicitly opt out of banner display. Ok.*/
			{
				BootBanner.BannerText[0] = '\0';
				BootBanner.BannerColor[0] = '\0';
				BootBanner.ShowBanner = false; /*Should already be false, but to prevent possible bugs...*/
				continue;
			}
			snprintf(BootBanner.BannerText, MAX_LINE_SIZE, "%s", DelimCurr);
			
			BootBanner.ShowBanner = true;
			
			if ((strlen(DelimCurr) + 1) >= MAX_DESCRIPT_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "BootBannerColor"), strlen("BootBannerColor")))
		{ /*Color for boot banner.*/
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!strcmp(DelimCurr, "NONE")) /*They don't want a color.*/
			{
				BootBanner.BannerColor[0] = '\0';
				continue;
			}
			
			SetBannerColor(DelimCurr); /*Function to be found elsewhere will do this for us, otherwise this loop would be even bigger.*/
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "DefaultRunlevel"), strlen("DefaultRunlevel")))
		{
			if (CurRunlevel[0] != 0)
			{ /*If the runlevel has already been set, don't set it again.
				* This prevents a rather nasty bug.*/
				continue;
			}
			
			if (CurObj != NULL)
			{ /*What the warning says. It'd get all weird if we allowed that.*/
				ConfigProblem(CONFIG_EAFTER, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}	
			
			snprintf(CurRunlevel, MAX_DESCRIPT_SIZE, "%s", DelimCurr);
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "Hostname"), strlen("Hostname")))
		{
			if (CurObj != NULL)
			{ /*What the warning says. It'd get all weird if we allowed that.*/
				ConfigProblem(CONFIG_EAFTER, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;

			}
			
			if (!strncmp(DelimCurr, "FILE", strlen("FILE")))
			{
				FILE *TDesc;
				unsigned long Inc = 0;
				short TChar;
				const char *TW = DelimCurr;
				char THostname[MAX_LINE_SIZE];
				
				TW += strlen("FILE");
				
				for (; *TW == ' ' || *TW == '\t'; ++TW);
				
				if (!(TDesc = fopen(TW, "r")))
				{
										snprintf(ErrBuf, sizeof ErrBuf, "Failed to set hostname from file \"%s\".\n", TW);
					SpitWarning(ErrBuf);
					continue;
				}
				
				for (Inc = 0; (TChar = getc(TDesc)) != EOF && Inc < MAX_LINE_SIZE - 1; ++Inc)
				{
					THostname[Inc] = (char)TChar;
				}
				THostname[Inc] = '\0';
				
				/*Skip past spaces, tabs, and newlines.*/
				for (TW = THostname; *TW == '\n' ||
					*TW == ' ' || *TW == '\t'; ++TW);
				
				/*Copy into the real hostname from our new offset.*/
				for (Inc = 0; TW[Inc] != '\0' && TW[Inc] != '\n'; ++Inc)
				{
					Hostname[Inc] = TW[Inc];
				}
				Hostname[Inc] = '\0';
				
				fclose(TDesc);
			}
			else
			{	
				snprintf(Hostname, MAX_LINE_SIZE, "%s", DelimCurr);
			}
			
								
			/*Check for spaces and tabs in the actual hostname.*/
			if (strstr(Hostname, " ") != NULL || strstr(Hostname, "\t") != NULL)
			{
				SpitWarning("Tabs and/or spaces in hostname file. Cannot set hostname.");
				*Hostname = '\0'; /*Set the hostname back to nothing.*/
				continue;
			}
			
			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}		
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectID"), strlen("ObjectID")))
		{ /*ASCII value used to identify this object internally, and also a kind of short name for it.*/
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}

			CurObj = AddObjectToTable(DelimCurr); /*Sets this as our current object.*/

			if ((strlen(DelimCurr) + 1) >= MAX_DESCRIPT_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}

			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectEnabled"), strlen("ObjectEnabled")))
		{
			if (!CurObj)
			{

				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!strcmp(DelimCurr, "true"))
			{
				CurObj->Enabled = true;
			}
			else if (!strcmp(DelimCurr, "false"))
			{
				CurObj->Enabled = false;
			}
			else
			{
				ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectOptions"), strlen("ObjectOptions")))
		{
			const char *TWorker = DelimCurr;
			unsigned long Inc;
			char CurArg[MAX_DESCRIPT_SIZE];
			
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			do
			{
				
				for (Inc = 0; TWorker[Inc] != ' ' && TWorker[Inc] != '\t' && TWorker[Inc] != '\n'
					&& TWorker[Inc] != '\0' && Inc < (MAX_DESCRIPT_SIZE - 1); ++Inc)
				{
					CurArg[Inc] = TWorker[Inc];
				}
				CurArg[Inc] = '\0';
				
				if (!strcmp(CurArg, "NOWAIT"))
				{
					CurObj->Opts.EmulNoWait = true;
					snprintf(ErrBuf, sizeof ErrBuf, "Option NOWAIT is deprecated and has been partially removed.\n"
							"Emulating NOWAIT for object %s.\nLine %lu in epoch.conf", CurObj->ObjectID, LineNum);
					SpitWarning(ErrBuf);
				}
				else if (!strcmp(CurArg, "HALTONLY"))
				{ /*Allow entries that execute on shutdown only.*/
					CurObj->Started = true;
					CurObj->Opts.CanStop = false;
					CurObj->Opts.HaltCmdOnly = true;
				}
				else if (!strcmp(CurArg, "PERSISTENT"))
				{
					CurObj->Opts.CanStop = false;
				}
				else if (!strcmp(CurArg, "RAWDESCRIPTION"))
				{
					CurObj->Opts.RawDescription = true;
				}
				else if (!strcmp(CurArg, "SERVICE"))
				{
					CurObj->Opts.IsService = true;
				}
				else if (!strcmp(CurArg, "AUTORESTART"))
				{
					CurObj->Opts.AutoRestart = true;
				}
				else if (!strcmp(CurArg, "FORCESHELL"))
				{
					if (!ShellEnabled)
					{
						snprintf(ErrBuf, sizeof ErrBuf, "Object %s has FORCESHELL set, but ShellEnabled is false.\n"
														"Ignoring.\nepoch.conf line %lu", CurObj->ObjectID, LineNum);
						SpitWarning(ErrBuf);
					}
					else
					{
						CurObj->Opts.ForceShell = true;
					}
				}
				else if (!strncmp(CurArg, "TERMSIGNAL", strlen("TERMSIGNAL")))
				{
					const char *TWorker = CurArg + strlen("TERMSIGNAL");
					
					if (*TWorker != '=' || *(TWorker + 1) == '\0')
					{
						ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, CurArg, LineNum);
						continue;
					}
					
					++TWorker;
					
					if (AllNumeric(TWorker))
					{
						if (atoi(TWorker) > 255)
						{
							ConfigProblem(CONFIG_ELARGENUM, CurArg, NULL, LineNum);
						}

						CurObj->TermSignal = atoi(TWorker);
					}
					else if (!strcmp("SIGTERM", TWorker))
					{
						CurObj->TermSignal = SIGTERM;
					}
					else if (!strcmp("SIGKILL", TWorker))
					{
						CurObj->TermSignal = SIGKILL;
					}
					else if (!strcmp("SIGHUP", TWorker))
					{
						CurObj->TermSignal = SIGKILL;
					}
					else if (!strcmp("SIGINT", TWorker))
					{
						CurObj->TermSignal = SIGINT;
					}
					else if (!strcmp("SIGABRT", TWorker))
					{
						CurObj->TermSignal = SIGABRT;
					}
					else if (!strcmp("SIGQUIT", TWorker))
					{
						CurObj->TermSignal = SIGQUIT;
					}
					else if (!strcmp("SIGUSR1", TWorker))
					{
						CurObj->TermSignal = SIGUSR1;
					}
					else if (!strcmp("SIGUSR2", TWorker))
					{
						CurObj->TermSignal = SIGUSR2;
					}
					else
					{
						ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, TWorker, LineNum);
						continue;
					}
				}
				else
				{
					ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, CurArg, LineNum);
					break;
				}
			} while ((TWorker = WhitespaceArg(TWorker)));
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectDescription"), strlen("ObjectDescription")))
		{ /*It's description.*/
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			snprintf(CurObj->ObjectDescription, MAX_DESCRIPT_SIZE, "%s", DelimCurr);
			
			if ((strlen(DelimCurr) + 1) >= MAX_DESCRIPT_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}

			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectStartCommand"), strlen("ObjectStartCommand")))
		{ /*What we execute to start it.*/
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			snprintf(CurObj->ObjectStartCommand, MAX_LINE_SIZE, "%s", DelimCurr);

			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectReloadCommand"), strlen("ObjectReloadCommand")))
		{
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			snprintf(CurObj->ObjectReloadCommand, MAX_LINE_SIZE, "%s", DelimCurr);
			
			if (strlen(DelimCurr) + 1 >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectStopCommand"), strlen("ObjectStopCommand")))
		{ /*If it's "PID", then we know that we need to kill the process ID only. If it's "NONE", well, self explanitory.*/
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}

			if (!strncmp(DelimCurr, "PIDFILE", strlen("PIDFILE")))
			{ /*They want us to kill a PID file on exit.*/
				const char *Worker = DelimCurr;
				
				Worker += strlen("PIDFILE");
				
				while (*Worker == ' ' || *Worker == '\t')
				{ /*Skip past all spaces and tabs.*/
					++Worker;
				}
				
				snprintf(CurObj->ObjectPIDFile, MAX_LINE_SIZE, "%s", Worker);
				
				CurObj->Opts.StopMode = STOP_PIDFILE;
			}
			else if (!strncmp(DelimCurr, "PID", strlen("PID")))
			{
				CurObj->Opts.StopMode = STOP_PID;
			}
			else if (!strncmp(DelimCurr, "NONE", strlen("NONE")))
			{
				CurObj->Opts.StopMode = STOP_NONE;
			}
			else
			{
				CurObj->Opts.StopMode = STOP_COMMAND;
				snprintf(CurObj->ObjectStopCommand, MAX_LINE_SIZE, "%s", DelimCurr);
			}
			
			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectStartPriority"), strlen("ObjectStartPriority")))
		{
			/*The order in which this item is started. If it is disabled in this runlevel, the next object in line is executed, IF
			 * and only IF it is enabled. If not, the one after that and so on.*/
			
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!AllNumeric(DelimCurr)) /*Make sure we are getting a number, not Shakespeare.*/
			{ /*No number? We're probably looking at an alias.*/
				unsigned long TmpTarget = 0;
				
				if (!(TmpTarget = PriorityAlias_Lookup(DelimCurr)))
				{
					ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
					continue;
				}
				
				CurObj->ObjectStartPriority = TmpTarget;
				continue;
			}
			
			CurObj->ObjectStartPriority = atoi(DelimCurr);
			
			if (strlen(DelimCurr) >= 8)
			{ /*An eight digit number is too high.*/
				ConfigProblem(CONFIG_ELARGENUM, CurrentAttribute, NULL, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectStopPriority"), strlen("ObjectStopPriority")))
		{
			/*Same as above, but used for when the object is being shut down.*/
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (!AllNumeric(DelimCurr))
			{
				unsigned long TmpTarget = 0;
				
				if (!(TmpTarget = PriorityAlias_Lookup(DelimCurr)))
				{
					ConfigProblem(CONFIG_EBADVAL, CurrentAttribute, DelimCurr, LineNum);
					continue;
				}
				
				CurObj->ObjectStopPriority = TmpTarget;
				continue;
			}
			
			CurObj->ObjectStopPriority = atoi(DelimCurr);
			
			if (strlen(DelimCurr) >= 8)
			{ /*An eight digit number is too high.*/
				ConfigProblem(CONFIG_ELARGENUM, CurrentAttribute, NULL, LineNum);
			}
			
			continue;
		}
		else if (!strncmp(Worker, (CurrentAttribute = "ObjectRunlevels"), strlen("ObjectRunlevels")))
		{ /*Runlevel.*/
			char *TWorker;
			char TRL[MAX_DESCRIPT_SIZE], *TRL2;
			static const ObjTable *LastObject = NULL;
			
			if (!CurObj)
			{
				ConfigProblem(CONFIG_EBEFORE, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			if (CurObj == LastObject)
			{ /*We cannot have multiple runlevel attributes because it messes up config file editing.*/
				snprintf(ErrBuf, sizeof ErrBuf, "Object %s has more than one ObjectRunlevels line.\n"
						"This is not advised because the config file editing code is not smart enough\n"
						"to handle multiple lines. You should put the additional runlevels on the same line.\n"
						"Line %lu in epoch.conf",
						CurObj->ObjectID, LineNum);
				SpitWarning(ErrBuf);
			}
			LastObject = CurObj;
			
			if (!GetLineDelim(Worker, DelimCurr))
			{
				ConfigProblem(CONFIG_EMISSINGVAL, CurrentAttribute, NULL, LineNum);
				continue;
			}
			
			TWorker = DelimCurr;
			
			do
			{
				for (TRL2 = TRL; *TWorker != ' ' && *TWorker != '\t' && *TWorker != '\n' && *TWorker != '\0'; ++TWorker, ++TRL2)
				{
					*TRL2 = *TWorker;
				}
				*TRL2 = '\0';
				
				ObjRL_AddRunlevel(TRL, CurObj);
				
			} while ((TWorker = WhitespaceArg(TWorker)));
			
			if ((strlen(DelimCurr) + 1) >= MAX_LINE_SIZE)
			{
				ConfigProblem(CONFIG_ETRUNCATED, CurrentAttribute, DelimCurr, LineNum);
			}
			
			continue;

		}
		else
		{ /*No big deal.*/
						snprintf(ErrBuf, sizeof ErrBuf, "Unidentified attribute in epoch.conf on line %lu.", LineNum);
			SpitWarning(ErrBuf);
			
			continue;
		}
	} while (++LineNum, (Worker = NextLine(Worker)));
	
	/*This code permits the usage of objects with the same priority by making them NOT
	 * the same priority, because the rest of the object handling subsystem is too stupid
	 * to know how to handle this, and this was easier to write.*/
	 for (ObjWorker = ObjectTable; ObjWorker->Next != NULL; ObjWorker = ObjWorker->Next)
	 {
		 for (CurObj = ObjectTable; CurObj->Next != NULL; CurObj = CurObj->Next)
		 {
			 if (ObjWorker->ObjectStartPriority != 0 && ObjWorker != CurObj &&
				CurObj->ObjectStartPriority == ObjWorker->ObjectStartPriority)
			{
				ObjTable *TWorker = ObjectTable;
				
				++CurObj->ObjectStartPriority;
				for (; TWorker->Next != NULL; TWorker = TWorker->Next)
				{
					if (TWorker->ObjectStartPriority >= CurObj->ObjectStartPriority &&
						CurObj != TWorker && TWorker != ObjWorker)
					{
						++TWorker->ObjectStartPriority;
					}
				}
			}
			
			if (ObjWorker->ObjectStopPriority != 0 && ObjWorker != CurObj &&
				CurObj->ObjectStopPriority == ObjWorker->ObjectStopPriority)
			{
				ObjTable *TWorker = ObjectTable;
				
				++CurObj->ObjectStopPriority;
				for (; TWorker->Next != NULL; TWorker = TWorker->Next)
				{
					if (TWorker->ObjectStopPriority >= CurObj->ObjectStopPriority &&
						CurObj != TWorker && TWorker != ObjWorker)
					{
						++TWorker->ObjectStopPriority;
					}
				}
			}
		}
	}
	
	/*NOWAIT is deprecated, so emulate it's effect with an ampersand.*/
	for (ObjWorker = ObjectTable; ObjWorker->Next; ObjWorker = ObjWorker->Next)
	{
		if (ObjWorker->Opts.EmulNoWait)
		{
			unsigned long TInc = 0;
			
			if (*ObjWorker->ObjectStartCommand == '\0')
			{ /*Don't bother if it's empty.*/
				continue;
			}
			
			/*Check if we already have an ampersand at the end.*/
			
			/*Go back behind any whitespace at the end.*/
			for (TInc = strlen(ObjWorker->ObjectStartCommand) - 1; ObjWorker->ObjectStartCommand[TInc] == ' ' ||
				ObjWorker->ObjectStartCommand[TInc] == '\t'; --TInc);
			
			if (ObjWorker->ObjectStartCommand[TInc] != '&')
			{
				strncat(ObjWorker->ObjectStartCommand, "&", MAX_LINE_SIZE - strlen(ObjWorker->ObjectStartCommand) - 1);
			}
		}
	}
	
	/*This is harmless, but it's bad form and could indicate human error in writing the config file.*/
	if (LongComment)
	{
				snprintf(ErrBuf, sizeof ErrBuf, "No comment terminator at end of configuration file.");
		SpitWarning(ErrBuf);
	}
	
	PriorityAlias_Shutdown(); /*We don't need to keep this in memory.*/
	
	switch (ScanConfigIntegrity())
	{
		case SUCCESS:
			break;
		case FAILURE:
		{ /*We failed integrity checking.*/
			fprintf(stderr, "%s\n", "Enter \"d\" to dump epoch.conf to console or strike enter to continue.\n->");
			fflush(NULL); /*Have an eerie feeling this will be necessary on some systems.*/
			
			if (getchar() == 'd')
			{
				fprintf(stderr, CONSOLE_COLOR_MAGENTA "Beginning dump of epoch.conf to console.\n" CONSOLE_ENDCOLOR);
				fprintf(stderr, "%s", ConfigStream);
				fflush(NULL);
			}
			else
			{
				puts("Not dumping epoch.conf.");
			}
			
			ShutdownConfig();
			free(ConfigStream);
			
			return FAILURE;
		}
		case WARNING:
		{
			SpitWarning("Noncritical configuration problems exist.\nPlease edit epoch.conf to resolve these.");
			return WARNING;
		}
	}
		
	free(ConfigStream); /*Release ConfigStream, since we only use the object table now.*/

	return SUCCESS;
}

static rStatus GetLineDelim(const char *InStream, char *OutStream)
{
	unsigned long cOffset, Inc = 0;

	/*Jump to the first tab or space. If we get a newline or null, problem.*/
	while (InStream[Inc] != '\t' && InStream[Inc] != ' ' && InStream[Inc] != '=' &&
			InStream[Inc] != '\n' && InStream[Inc] != '\0') ++Inc;

	/*Hit a null or newline before tab or space. ***BAD!!!*** */
	if (InStream[Inc] == '\0' || InStream[Inc] == '\n')
	{
		char TmpBuf[1024];
		char ObjectInQuestion[1024];
		unsigned long IncT = 0;

		for (; InStream[IncT] != '\0' && InStream[IncT] != '\n'; ++IncT)
		{
			ObjectInQuestion[IncT] = InStream[IncT];
		}
		ObjectInQuestion[IncT] = '\0';

		snprintf(TmpBuf, 1024, "No parameter for attribute \"%s\" in epoch.conf.", ObjectInQuestion);

		SpitError(TmpBuf);

		return FAILURE;
	}
	
	if (InStream[Inc] == '=')
	{ /*We give the choice of using whitespace or using an equals sign. It's only nice.*/
		++Inc;
	}
	else
	{
		/*Continue until we are past all tabs and spaces.*/
		while (InStream[Inc] == ' ' || InStream[Inc] == '\t') ++Inc;
	}

	cOffset = Inc; /*Store this offset.*/

	/*Copy over the argument to the parameter. Quit whining about the loop copy.*/
	for (Inc = 0; InStream[Inc + cOffset] != '\n' && InStream[Inc + cOffset] != '\0' && Inc < MAX_LINE_SIZE - 1; ++Inc)
	{
		OutStream[Inc] = InStream[Inc + cOffset];
	}
	OutStream[Inc] = '\0';

	return SUCCESS;
}

rStatus EditConfigValue(const char *ObjectID, const char *Attribute, const char *Value)
{ /*Looks up the attribute for the passed ID and replaces the value for that attribute.*/
	char *MasterStream = NULL, *HalfTwo = NULL;
	char *NewValue = NULL, *Worker = NULL, *Stopper = NULL, *LineArm = NULL;
	char LineWorkerR[MAX_LINE_SIZE];
	FILE *Descriptor = NULL;
	char *WhiteSpace = NULL, *LineWorker = NULL;
	struct stat FileStat;
	unsigned long Inc = 0, Inc2 = 0, LineNum = 1;
	unsigned long NumWhiteSpaces = 0;
	Bool PresentHalfTwo = false;
	
	if (stat(CONFIGDIR CONF_NAME, &FileStat) != 0)
	{
		SpitError("EditConfigValue(): Failed to stat " CONFIGDIR CONF_NAME ". Does the file exist?");
		return FAILURE;
	}
	
	if ((Descriptor = fopen(CONFIGDIR CONF_NAME, "r")) == NULL)
	{
		SpitError("EditConfigValue(): Failed to open " CONFIGDIR CONF_NAME ". Are permissions correct?");
		return FAILURE;
	}
	
	MasterStream = malloc(FileStat.st_size + 1);
	
	/*Read in the file.*/
	fread(MasterStream, 1, FileStat.st_size, Descriptor);
	MasterStream[FileStat.st_size] = '\0';
	
	/*We don't need this anymore.*/
	fclose(Descriptor);
	
	if (*MasterStream == '\0')
	{
		free(MasterStream);
		return FAILURE;
	}

	/*Erase the newlines at the end of the file so we don't need to mess with them later.*/
	for (; MasterStream[Inc2] != '\0'; ++Inc2);
	
	for (; MasterStream[Inc2] == '\n'; --Inc2)
	{
		MasterStream[Inc2] = '\0';
	}
	
	/*Find the object ID of our object in config.*/
	LineArm = MasterStream; do
	{
		for (Inc = 0; LineArm[Inc] != '\n' && LineArm[Inc] != '\0'; ++Inc)
		{
			LineWorkerR[Inc] = LineArm[Inc];
		}
		LineWorkerR[Inc] = '\0';
		
		/*Skip past any whitespace at the beginning of the line.*/
		for (Inc2 = 0; LineWorkerR[Inc2] == ' ' || LineWorkerR[Inc2] == '\t'; ++Inc2);
		
		LineWorker = &LineWorkerR[Inc2];
		
		if (strncmp(LineWorker, "ObjectID", strlen("ObjectID")) != 0)
		{ /*Not ObjectID?*/
			continue;
		}
		
		/*Assume we found an ObjectID beyond here.*/
		
		/*Get to the beginning of the whitespace. We use Inc2 as a marker for it's beginning*/
		for (Inc2 = 0; LineWorker[Inc2] != ' ' && LineWorker[Inc2] != '\t' &&
			LineWorker[Inc2] != '=' && LineWorker[Inc2] != '\0' && LineWorker[Inc2] != '\n'; ++Inc2);
		
		if (LineWorker[Inc2] == '=')
		{ /* We allow both spaces and whitespace as the delimiter.*/
			NumWhiteSpaces = 1;
		}
		else
		{ /*Count the number of whitespaces.*/
			for (NumWhiteSpaces = 0 ; LineWorker[NumWhiteSpaces + Inc2] == ' ' ||
				LineWorker[NumWhiteSpaces + Inc2] == '\t'; ++NumWhiteSpaces);
		}
			
		if (LineWorker[NumWhiteSpaces + Inc2] == '\0' ||
			LineWorker[NumWhiteSpaces + Inc2] == '\n')
		{ /*Malformed config lines cannot be edited.*/
			free(MasterStream);
			return FAILURE;
		}
		
		if (strcmp(&LineWorker[NumWhiteSpaces + Inc2], ObjectID) != 0)
		{ /*Not the ObjectID we were looking for?*/
			continue;
		}
		
		/*We found it! Save the pointer.*/
		Worker = LineArm + Inc2 + NumWhiteSpaces + strlen(ObjectID);
	} while (++LineNum, (LineArm = NextLine(LineArm)));
	
	if (Worker == NULL)
	{ /*If we didn't find it.*/
		free(MasterStream);
		return FAILURE;
	}
	
	/*Do not accidentally jump to a different object's attribute of the same name.*/
	if ((Stopper = strstr(Worker, "ObjectID")) != NULL)
	{
		*Stopper = '\0';
	}
	
	if (!(Worker = strstr(Worker, Attribute)) || (Worker > &LineArm[0] && *(Worker - 1) == '#'))
	{ /*Doesn't exist for that object? We also ignore comments.*/
		free(MasterStream);
		return FAILURE;
	}
	
	if (Stopper) *Stopper = 'O'; /*set back, use capital O.*/
	
	/*Null-terminate half one.*/
	*Worker++ = '\0';
	
	/*Jump to the whitespace.*/
	for (Inc = 0; Worker[Inc] != ' ' && Worker[Inc] != '\t' &&
		Worker[Inc] != '=' && Worker[Inc] != '\n' && Worker[Inc] != '\0'; ++Inc);
		
	if (Worker[Inc] == '\n' || Worker[Inc] == '\0')
	{ /*Malformed line. Can't edit it.*/
		free(MasterStream);
		return FAILURE;
	}
	
	if (Worker[Inc] == '=')
	{
		NumWhiteSpaces = 1;
		Worker += Inc;
	}
	else
	{ /*Count the whitespace.*/
		for (NumWhiteSpaces = 0, Worker += Inc; Worker[NumWhiteSpaces] == ' ' ||
			Worker[NumWhiteSpaces] == '\t'; ++NumWhiteSpaces);
	}
	
	WhiteSpace = malloc(NumWhiteSpaces + 1);
	
	/*Save the whitespace while incrementing Worker to the value of this line at the same time.*/
	for (Inc2 = 0; *Worker == '=' || *Worker == ' ' || *Worker == '\t'; ++Inc2, ++Worker)
	{
		WhiteSpace[Inc2] = *Worker;
	}
	WhiteSpace[Inc2] = '\0';
	
	/*Jump past the newline on the end of this line.*/
	for (; *Worker != '\n' && *Worker != '\0'; ++Worker);
	
	if (*Worker != '\0')
	{ /*There is more beyond this line.*/
		PresentHalfTwo = true;
		HalfTwo = malloc(strlen(Worker) + 1);
		
		strncpy(HalfTwo, Worker, strlen(Worker) + 1); /*Plus one to copy the null terminator.*/
		
	}
	
	/*Set up the new value.*/
	NewValue = malloc(strlen(Attribute) + NumWhiteSpaces + strlen(Value) + 1);
	snprintf(NewValue, (strlen(Attribute) + NumWhiteSpaces + strlen(Value) + 1),
			"%s%s%s", Attribute, WhiteSpace, Value);
	
	/*Wwe copied the whitespace back into the new value, so release it's memory now.*/
	free(WhiteSpace);
	
	/*Reallocate MasterStream to accomodate the new data.*/
	MasterStream = realloc(MasterStream, strlen(MasterStream) + strlen(NewValue) +
							(PresentHalfTwo ? strlen(HalfTwo) : 0) + 1);
							
	/*Copy in the new string.*/
	snprintf( (MasterStream + strlen(MasterStream)), (strlen(MasterStream) +
				strlen(NewValue) +(PresentHalfTwo ? strlen(HalfTwo) : 0) + 1),
				"%s%s", NewValue, (PresentHalfTwo ? HalfTwo : ""));
				
	/*Release the other variables now that we don't need them.*/
	free(NewValue);
	free(HalfTwo);
	
	/*Write the configuration back to disk.*/
	Descriptor = fopen(CONFIGDIR CONF_NAME, "w");
	fwrite(MasterStream, 1, strlen(MasterStream), Descriptor);
	fclose(Descriptor);
	
	/*Release MasterStream.*/
	free(MasterStream);
	
	return SUCCESS;
}

/*Adds an object to the table and, if the first run, sets up the table.*/
static ObjTable *AddObjectToTable(const char *ObjectID)
{
	ObjTable *Worker = ObjectTable;
	
	/*See, we actually allocate two cells initially. The base and it's node.
	 * We always keep a free one open. This is just more convenient.*/
	if (ObjectTable == NULL)
	{
		ObjectTable = malloc(sizeof(ObjTable));
		ObjectTable->Prev = NULL;
		ObjectTable->Next = NULL;

		Worker = ObjectTable;
	}
	
	while (Worker->Next)
	{
		Worker = Worker->Next;
	}

	Worker->Next = malloc(sizeof(ObjTable));
	Worker->Next->Next = NULL;
	Worker->Next->Prev = Worker;

	/*This is the first thing that must ever be initialized, because it's how we tell objects apart.*/
	snprintf(Worker->ObjectID, MAX_DESCRIPT_SIZE, "%s", ObjectID);
	
	/*Initialize these to their default values. Used to test integrity before execution begins.*/
	Worker->Started = false;
	Worker->ObjectDescription[0] = '\0';
	Worker->ObjectStartCommand[0] = '\0';
	Worker->ObjectStopCommand[0] = '\0';
	Worker->ObjectReloadCommand[0] = '\0';
	Worker->ObjectPIDFile[0] = '\0';
	Worker->ObjectStartPriority = 0;
	Worker->ObjectStopPriority = 0;
	Worker->Opts.StopMode = STOP_NONE;
	Worker->Opts.CanStop = true;
	Worker->ObjectPID = 0;
	Worker->TermSignal = SIGTERM; /*This can be changed via config.*/
	Worker->ObjectRunlevels = NULL;
	Worker->Enabled = 2; /*We can indeed store this in a bool you know. There's no 1 bit datatype.*/
	Worker->Opts.HaltCmdOnly = false;
	Worker->Opts.RawDescription = false;
	Worker->Opts.IsService = false;
	Worker->Opts.AutoRestart = false;
	Worker->Opts.EmulNoWait = false;
	Worker->Opts.ForceShell = false;
	
	return Worker;
}

static rStatus ScanConfigIntegrity(void)
{ /*Here we check common mistakes and problems.*/
	ObjTable *Worker = ObjectTable, *TOffender;
	char TmpBuf[1024];
	rStatus RetState = SUCCESS;
	static Bool WasRunBefore = false;
	
	if (ObjectTable == NULL)
	{ /*This can happen if configuration is filled with trash and nothing valid.*/
		SpitError("No objects found in configuration or invalid configuration.");
		return FAILURE;
	}
	
	if (*CurRunlevel == 0 || !ObjRL_ValidRunlevel(CurRunlevel))
	{	
		if (*CurRunlevel == 0)
		{
			SpitError("No default runlevel specified!");
		}
		else
		{
	
			snprintf(TmpBuf, sizeof TmpBuf, "%sThe runlevel \"%s\" does not exist.",
					WasRunBefore ? "A problem has occured in configuration.\n" : "Error booting to default runlevel.\n",
					CurRunlevel);

			SpitError(TmpBuf);
			
			if (WasRunBefore)
			{
				puts("Switch to an existing runlevel and then try to reload the configuration again.");
			}
		}
		
		if (!WasRunBefore)
		{ /*We can ask for a new runlevel if we are just booting, otherwise the other is restored by ReloadConfig().*/
			char NewRL[MAX_DESCRIPT_SIZE];
			Bool BadRL = true;

			do
			{
				printf("Please enter a valid runlevel to continue\n"
						"or strike enter to go to an emergency shell.\n\n--> ");
				
				fgets(NewRL, MAX_DESCRIPT_SIZE, stdin);
				
				if (NewRL[0] == '\n')
				{
					puts("Starting emergency shell as per your request.");
					EmergencyShell();
				}
				
				NewRL[strlen(NewRL) - 1] = '\0'; /*nuke the newline at the end.*/
				
				if (ObjRL_ValidRunlevel(NewRL))
				{
					puts("Runlevel accepted.\n");
					BadRL = false;
	
					snprintf(CurRunlevel, MAX_DESCRIPT_SIZE, "%s", NewRL);
				}
				
				if (BadRL) SmallError("The runlevel you entered was not found. Please try again.\n");
			} while (BadRL);
		}
		else
		{
			return FAILURE;
		}
			
	}
	
	for (; Worker->Next != NULL; Worker = Worker->Next)
	{
		if (*Worker->ObjectDescription == '\0')
		{
			snprintf(TmpBuf, 1024, "Object %s has no attribute ObjectDescription.\n"
						"Changing description to \"missing description\".", Worker->ObjectID);
			SpitWarning(TmpBuf);
			
			snprintf(Worker->ObjectDescription, MAX_DESCRIPT_SIZE, "%s", 
					CONSOLE_COLOR_YELLOW "[missing description]" CONSOLE_ENDCOLOR);

			RetState = WARNING;
		}
		
		if (*Worker->ObjectStartCommand == '\0' && *Worker->ObjectStopCommand == '\0' && Worker->Opts.StopMode == STOP_COMMAND)
		{
			snprintf(TmpBuf, 1024, "Object %s has neither ObjectStopCommand nor ObjectStartCommand attributes.", Worker->ObjectID);
			SpitError(TmpBuf);
			RetState = FAILURE;
		}
		
		if (!Worker->Opts.HaltCmdOnly && *Worker->ObjectStartCommand == '\0')
		{
			snprintf(TmpBuf, 1024, "Object %s has no attribute ObjectStartCommand\nand is not set to HALTONLY.\n"
					"Disabling.", Worker->ObjectID);
			SpitWarning(TmpBuf);
			Worker->Enabled = false;
			RetState = WARNING;
		}
		
		if (Worker->ObjectRunlevels == NULL && !Worker->Opts.HaltCmdOnly)
		{
			snprintf(TmpBuf, 1024, "Object \"%s\" has no attribute ObjectRunlevels.", Worker->ObjectID);
			SpitError(TmpBuf);
			RetState = FAILURE;
		}
		
		if (Worker->Enabled == 2)
		{
			snprintf(TmpBuf, 1024, "Object \"%s\" has no attribute ObjectEnabled.", Worker->ObjectID);
			SpitError(TmpBuf);
			RetState = FAILURE;
		}
		
		if (Worker->Opts.StopMode == STOP_PID && Worker->Opts.HaltCmdOnly)
		{ /*We put this here instead of InitConfig() because we can't really do anything but disable.*/
			snprintf(TmpBuf, 1024, "Object \"%s\" has HALTONLY set,\n"
					"but stop method is PID!\nDisabling.", Worker->ObjectID);
			SpitWarning(TmpBuf);
			Worker->Enabled = false;
			RetState = WARNING;
		}
		
		/*Check for duplicate ObjectIDs.*/
		for (TOffender = ObjectTable; TOffender->Next != NULL; TOffender = TOffender->Next)
		{
			if (!strcmp(Worker->ObjectID, TOffender->ObjectID) && Worker != TOffender)
			{
				snprintf(TmpBuf, 1024, "Two objects in configuration with ObjectID \"%s\".", Worker->ObjectID);
				SpitError(TmpBuf);
				RetState = FAILURE;
			}			
		}
	}
	
	WasRunBefore = true;
	
	return RetState;
}
	
/*Find an object in the table and return a pointer to it. This function is public
 * because while we don't want other places adding to the table, we do want read
 * access to the table.*/
ObjTable *LookupObjectInTable(const char *ObjectID)
{
	ObjTable *Worker = ObjectTable;

	if (!ObjectTable)
	{
		return NULL;
	}
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		if (!strcmp(Worker->ObjectID, ObjectID))
		{
			return Worker;
		}
	}

	return NULL;
}

/*Get the max priority number we need to scan.*/
unsigned long GetHighestPriority(Bool WantStartPriority)
{
	ObjTable *Worker = ObjectTable;
	unsigned long CurHighest = 0;
	unsigned long TempNum;
	
	if (!ObjectTable)
	{
		return 0;
	}
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		TempNum = (WantStartPriority ? Worker->ObjectStartPriority : Worker->ObjectStopPriority);
		
		if (TempNum > CurHighest)
		{
			CurHighest = TempNum;
		}
		else if (TempNum == 0)
		{ /*We always skip anything with a priority of zero. That's like saying "DISABLED".*/
			continue;
		}
	}
	
	return CurHighest;
}

/*Functions for runlevel management.*/
Bool ObjRL_CheckRunlevel(const char *InRL, const ObjTable *InObj, Bool CountInherited)
{
	struct _RLTree *Worker = InObj->ObjectRunlevels;
	Bool RetVal = false;
	
	if (Worker == NULL)
	{
		return false;
	}
	
	if (CountInherited)
	{
		for (; Worker->Next; Worker = Worker->Next)
		{
			if (RLInheritance_Check(InRL, Worker->RL))
			{
				RetVal = 2;
				break;
			}
		}
	}
	
	for (Worker = InObj->ObjectRunlevels; Worker->Next != NULL; Worker = Worker->Next)
	{
		if (!strcmp(Worker->RL, InRL))
		{
			return true;
		}
	}
	
	return RetVal;
}
	
void ObjRL_AddRunlevel(const char *InRL, ObjTable *InObj)
{
	struct _RLTree *Worker = InObj->ObjectRunlevels;
	
	if (InObj->ObjectRunlevels == NULL)
	{
		InObj->ObjectRunlevels = malloc(sizeof(struct _RLTree));
		
		InObj->ObjectRunlevels->Prev = NULL;
		InObj->ObjectRunlevels->Next = NULL;
		Worker = InObj->ObjectRunlevels;
	}
	
	while (Worker->Next != NULL) Worker = Worker->Next;
	
	Worker->Next = malloc(sizeof(struct _RLTree));
	Worker->Next->Next = NULL;
	Worker->Next->Prev = Worker;
	
	snprintf(Worker->RL, MAX_DESCRIPT_SIZE, "%s", InRL);
}

Bool ObjRL_DelRunlevel(const char *InRL, ObjTable *InObj)
{
	struct _RLTree *Worker = InObj->ObjectRunlevels;
	
	if (Worker == NULL)
	{
		return false;
	}
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		if (!strcmp(InRL, Worker->RL))
		{
			if (Worker == InObj->ObjectRunlevels)
			{ /*If it's the first node*/
				
				if (InObj->ObjectRunlevels->Next->Next != NULL)
				{ /*Are there other runlevels enabled, or just us?*/
					InObj->ObjectRunlevels->Next->Prev = NULL;
					InObj->ObjectRunlevels = InObj->ObjectRunlevels->Next;
					free(Worker);
				}
				else
				{ /*Apparently just us.*/
					ObjRL_ShutdownRunlevels(InObj);
				}
				
				return true;
			}
				
			/*Otherwise, do this.*/
			Worker->Prev->Next = Worker->Next;
			Worker->Next->Prev = Worker->Prev;	
				
			free(Worker);
			
			return true;
		}
	}
	
	return false;
}

Bool ObjRL_ValidRunlevel(const char *InRL)
{ /*checks if a runlevel has anything at all using it.*/
	const ObjTable *Worker = ObjectTable;
	Bool ValidRL = false;
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		if (!Worker->Opts.HaltCmdOnly && ObjRL_CheckRunlevel(InRL, Worker, true))
		{
			ValidRL = true;
			break;
		}
	}
	
	return ValidRL;
}

void ObjRL_ShutdownRunlevels(ObjTable *InObj)
{
	struct _RLTree *Worker = InObj->ObjectRunlevels, *NDel;
	
	for (; Worker != NULL; Worker = NDel)
	{
		NDel = Worker->Next;
		free(Worker);
	}
	
	InObj->ObjectRunlevels = NULL;
}

static void PriorityAlias_Add(const char *Alias, unsigned long Target)
{ /*This code should be simple enough. Just routine linked list stuff.*/
	struct _PriorityAliasTree *Worker = PriorityAliasTree;
	
	if (!PriorityAliasTree)
	{
		PriorityAliasTree = malloc(sizeof(struct _PriorityAliasTree));
		PriorityAliasTree->Next = NULL;
		PriorityAliasTree->Prev = NULL;
		Worker = PriorityAliasTree;
	}
	else
	{ /*If we are the first node, it's not possible for the object to already exist.*/
		for (; Worker->Next; Worker = Worker->Next)
		{
			if (!strcmp(Worker->Alias, Alias))
			{
				return;
			}
		}
	}
	
	Worker->Next = malloc(sizeof(struct _PriorityAliasTree));
	Worker->Next->Next = NULL;
	Worker->Next->Prev = Worker;
	
	strncpy(Worker->Alias, Alias, strlen(Alias) + 1);
	Worker->Target = Target;
}

static void PriorityAlias_Shutdown(void)
{
	struct _PriorityAliasTree *Worker = PriorityAliasTree, *TmpFree = NULL;
	
	if (!PriorityAliasTree) return;
	
	while (Worker != NULL)
	{
		TmpFree = Worker;
		Worker = Worker->Next;
		
		free(TmpFree);
	}
	
	PriorityAliasTree = NULL;
}

static unsigned long PriorityAlias_Lookup(const char *Alias)
{ /*Return 0 if we cannot find anything.*/
	struct _PriorityAliasTree *Worker = PriorityAliasTree;
	
	if (!Worker) return 0;
	
	for (; Worker->Next; Worker = Worker->Next)
	{
		if (!strcmp(Worker->Alias, Alias))
		{
			return Worker->Target;
		}
	}
	
	return 0;
}

static void RLInheritance_Add(const char *Inheriter, const char *Inherited)
{
	struct _RunlevelInheritance *Worker = RunlevelInheritance;
	
	if (!Worker)
	{
		RunlevelInheritance = malloc(sizeof(struct _RunlevelInheritance));
		memset(RunlevelInheritance, 0, sizeof(struct _RunlevelInheritance));
		
		Worker = RunlevelInheritance;
	}
	
	while (Worker->Next) Worker = Worker->Next;
	
	Worker->Next = malloc(sizeof(struct _RunlevelInheritance));
	memset(Worker->Next, 0, sizeof(struct _RunlevelInheritance));
	
	Worker->Next->Prev = Worker;
	
	strncpy(Worker->Inheriter, Inheriter, strlen(Inheriter) + 1);
	strncpy(Worker->Inherited, Inherited, strlen(Inherited) + 1);
	
}

static Bool RLInheritance_Check(const char *Inheriter, const char *Inherited)
{ /*Check if Inheriter inherits inherited.*/
	struct _RunlevelInheritance *Worker = RunlevelInheritance;
	
	if (!Worker) return false;
	
	for (; Worker->Next != NULL; Worker = Worker->Next)
	{
		if (!strcmp(Inheriter, Worker->Inheriter) && !strcmp(Inherited, Worker->Inherited))
		{
			return true;
		}
	}
	
	return false;
}

static void RLInheritance_Shutdown(void)
{
	struct _RunlevelInheritance *Worker = RunlevelInheritance, *TDel = NULL;
	
	for (; Worker != NULL; Worker = TDel)
	{
		TDel = Worker->Next;
		free(Worker);
	}
	
	RunlevelInheritance = NULL;
}

ObjTable *GetObjectByPriority(const char *ObjectRunlevel, Bool WantStartPriority, unsigned long ObjectPriority)
{ /*The primary lookup function to be used when executing commands.*/
	ObjTable *Worker = ObjectTable;
	
	if (!ObjectTable)
	{
		return NULL;
	}
	
	for (; Worker->Next != NULL; Worker = Worker->Next)
	{
		if ((ObjectRunlevel == NULL || ((WantStartPriority || !Worker->Opts.HaltCmdOnly) &&
			ObjRL_CheckRunlevel(ObjectRunlevel, Worker, true))) && 
			/*As you can see by below, I obfuscate with efficiency!*/
			(WantStartPriority ? Worker->ObjectStartPriority : Worker->ObjectStopPriority) == ObjectPriority)
		{
			return Worker;
		}
	}
	
	return NULL;
}

void ShutdownConfig(void)
{
	ObjTable *Worker = ObjectTable, *Temp;

	for (; Worker != NULL; Worker = Temp)
	{
		if (Worker->Next)
		{
			ObjRL_ShutdownRunlevels(Worker);
		}
		
		Temp = Worker->Next;
		free(Worker);
	}
	
	RLInheritance_Shutdown();
	
	ObjectTable = NULL;
}

rStatus ReloadConfig(void)
{ /*This function is somewhat hard to read, but it does the job well.*/
	ObjTable *Worker = ObjectTable;
	ObjTable *TRoot = malloc(sizeof(ObjTable)), *SWorker = TRoot, *Temp = NULL;
	struct _RLTree *RLTemp1 = NULL, *RLTemp2 = NULL;
	Bool GlobalTriple[3], ConfigOK = true;
	struct _RunlevelInheritance *RLIRoot = NULL, *RLIWorker[2] = { NULL };
	char RunlevelBackup[MAX_DESCRIPT_SIZE];
	
	WriteLogLine("CONFIG: Reloading configuration.\n", true);
	WriteLogLine("CONFIG: Backing up current configuration.", true);
	
	/*Backup the current runlevel.*/
	snprintf(RunlevelBackup, MAX_DESCRIPT_SIZE, "%s", CurRunlevel);
	
	for (; Worker->Next != NULL; Worker = Worker->Next, SWorker = SWorker->Next)
	{
		*SWorker = *Worker; /*Direct as-a-unit copy of the main list node to the backup list node.*/
		SWorker->Next = malloc(sizeof(ObjTable));
		SWorker->Next->Next = NULL;
		SWorker->Next->Prev = SWorker;
		
		if (!Worker->ObjectRunlevels)
		{
			continue;
		}
		
		RLTemp2 = SWorker->ObjectRunlevels = malloc(sizeof(struct _RLTree));
		
		for (RLTemp1 = Worker->ObjectRunlevels; RLTemp1->Next; RLTemp1 = RLTemp1->Next)
		{
			*RLTemp2 = *RLTemp1;
			RLTemp2->Next = malloc(sizeof(struct _RLTree));
			RLTemp2->Next->Next = NULL;
			RLTemp2->Next->Prev = RLTemp2;
			RLTemp2 = RLTemp2->Next;
		}
	}

	/*Back up the runlevel inheritance table.*/
	if (RunlevelInheritance != NULL)
	{
		RLIRoot = RLIWorker[1] = malloc(sizeof(struct _RunlevelInheritance));
		memset(RLIWorker[1], 0, sizeof(struct _RunlevelInheritance));
		
		for (RLIWorker[0] = RunlevelInheritance; RLIWorker[0]->Next; RLIWorker[0] = RLIWorker[0]->Next)
		{
			*RLIWorker[1] = *RLIWorker[0];
			
			RLIWorker[1]->Next = malloc(sizeof(struct _RunlevelInheritance));
			memset(RLIWorker[1]->Next, 0, sizeof(struct _RunlevelInheritance));
			
			RLIWorker[1]->Next->Prev = RLIWorker[1];
		}
	}
	
	WriteLogLine("CONFIG: Shutting down configuration.", true);
	
	/*Actually do the reload of the config.*/
	ShutdownConfig();
	
	/*Do this to prevent some weird options from being changeable by a config reload.*/
	GlobalTriple[0] = EnableLogging;
	GlobalTriple[1] = DisableCAD;
	GlobalTriple[2] = AlignStatusReports;
	
	WriteLogLine("CONFIG: Initializing new configuration.", true);
	
	if (!InitConfig())
	{
		WriteLogLine("CONFIG: " CONSOLE_COLOR_RED "FAILED TO RELOAD CONFIGURATION." CONSOLE_ENDCOLOR 
					" Restoring previous configuration from backup.", true);
		SpitError("ReloadConfig(): Failed to reload configuration.\n"
					"Restoring old configuration to memory.\n"
					"Please check epoch.conf for syntax errors.");
		ObjectTable = TRoot; /*Point ObjectTable to our new, identical copy of the old tree.*/
		
		/*Restore runlevel inheritance state.*/
		RLInheritance_Shutdown();
		RunlevelInheritance = RLIRoot;
		
		/*Restore current runlevel*/
		snprintf(CurRunlevel, MAX_DESCRIPT_SIZE, "%s", RunlevelBackup);
		
		ConfigOK = false;
	}
	
	/*And then restore those options to their previous states.*/
	EnableLogging = GlobalTriple[0];
	DisableCAD = GlobalTriple[1];
	AlignStatusReports = GlobalTriple[2];
	
	if (!ConfigOK) return ConfigOK;
	
	WriteLogLine("CONFIG: Restoring object statuses and deleting backup configuration.", true);
	
	for (SWorker = TRoot; SWorker->Next != NULL; SWorker = Temp)
	{ /*Add back the Started states, so we don't forget to stop services, etc.*/
		if ((Worker = LookupObjectInTable(SWorker->ObjectID)))
		{
			Worker->Started = SWorker->Started;
			Worker->ObjectPID = SWorker->ObjectPID;
		}
		
		ObjRL_ShutdownRunlevels(SWorker);
		Temp = SWorker->Next;
		free(SWorker);
	}
	free(SWorker);
	
	/*Release the runlevel inheritance table.*/
	for (; RLIRoot != NULL; RLIRoot = RLIWorker[0])
	{
		RLIWorker[0] = RLIRoot->Next;
		free(RLIRoot);
	}
	
	WriteLogLine("CONFIG: " CONSOLE_COLOR_GREEN "Configuration reload successful." CONSOLE_ENDCOLOR, true);
	puts(CONSOLE_COLOR_GREEN "Epoch: Configuration reloaded." CONSOLE_ENDCOLOR);
	
	return SUCCESS;
}
