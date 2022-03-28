// midi-carver.c - Greg Kennedy 2010
//  advanced data carver specifically for MIDI files
//  can extract MIDI files from a binary blob
// version 1.0

//  Will also attempt to reconstruct damaged data -
//   * orphaned series of MTrk will have a valid MThd applied
//   * rebuilds MIDI header when a "hole" of missing MTrks
//       is found after an MThd
//   * tries to properly terminate an incorrectly ended MTrk at the last
//       MIDI event, and restructures the file appropriately
//   * looks for an MThd or MTrk in the middle of a running MThd, and
//       creates two files

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "libgen.h"
#include "sys/stat.h"

static char out_dir[1000];

struct mthd
{
	unsigned short miditype,numtracks,timecode;
	unsigned char is_damaged,is_generated;
	struct mtrk *track0;
};

struct mtrk
{
	unsigned int size;
	unsigned char extratrunc, *data;
	struct mtrk *next;
};

void write_mtrk(struct mtrk *track, FILE *fp)
{
	unsigned char buffer[4];

	if (track == NULL || fp == NULL) return;

	// convert size to device-independent buffer
	buffer[0] = ((track->size) / 1677216);
	buffer[1] = (((track->size) % 1677216) / 65536);
	buffer[2] = (((track->size) % 65536) / 256);
	buffer[3] = ((track->size) % 256);

	// write track to disk
	fwrite("MTrk",1,4,fp);
	fwrite(buffer,1,4,fp);
	fwrite(track->data,1,track->size,fp);

	// recursively write next track
	if (track->next != NULL)
		write_mtrk(track->next,fp);

	// free current track
	free(track->data);
	free(track);
}

int write_midi(struct mthd *midi, const char *filename)
{
	FILE *fp = NULL;
	unsigned char buffer[6];

	if (midi->track0 == NULL)
	{
		printf(" Refusing to write trackless MIDI file.\n");
		return 1;
	}

	fp = fopen(filename,"wb");
	if (fp == NULL)
	{
		printf(" ERROR: could not open %s for writing!!\n",filename);
		return 1;
	}

	buffer[0] = ((midi->miditype) / 256);
	buffer[1] = ((midi->miditype) % 256);
	buffer[2] = ((midi->numtracks) / 256);
	buffer[3] = ((midi->numtracks) % 256);
	buffer[4] = ((midi->timecode) / 256);
	buffer[5] = ((midi->timecode) % 256);

	fwrite("MThd\0\0\0\x06",1,8,fp);
	fwrite(buffer,1,6,fp);

	write_mtrk(midi->track0,fp);

	fclose(fp);

	free(midi);

	printf(" Success!  Wrote %s to disk.\n",filename);

	return 0;
}

// Extracts an MThd from a block.
struct mthd *extract_mthd(unsigned char *buffer)
{
	struct mthd *newmidi = NULL;
	unsigned int ipointer;

// looks like a winner?
	if (strncmp((char *)buffer,"MThd",4) == 0)
	{
		newmidi = malloc(sizeof(struct mthd));
		newmidi->track0=NULL;

		newmidi->is_damaged=0;
		newmidi->is_generated=0;

// Looks like a MIDI file, let's check for consistency
// figure the buffer size - this is stupid of course as it should always be 6
//  but oh well.
		ipointer = (buffer[4] * 16777216) +
			(buffer[5] * 65536) +
			(buffer[6] * 256) +
			buffer[7];
		if (ipointer == 6) printf(" Header indicates 6 bytes length, that's good.\n"); else printf(" Header size says %d bytes - bad news, it should be 6.  Continuing anyway.\n",ipointer);

// Get the MIDI Type
		newmidi->miditype = buffer[8] * 256 + buffer[9];
		if (newmidi->miditype <= 2)
			printf(" MIDI file says it is type %hd\n",newmidi->miditype);
		else
			printf(" MIDI file is type %hd (should be 0-2).  Continuing anyway.\n",newmidi->miditype);

// Get the number of tracks
		newmidi->numtracks = buffer[10] * 256 + buffer[11];
		printf(" MIDI says there should be %hd tracks here.\n",newmidi->numtracks);

		if (newmidi->miditype == 0 && newmidi->numtracks != 1)
		{
			printf(" NOTE that type 0 should have only 1 track...?  Altering type to Type 1.\n");
			newmidi->miditype=1;
		}

// Get the timecode.  This can't really be verified.
		newmidi->timecode = buffer[12] * 256 + buffer[13];
		printf(" MIDI timecode:  %hd\n",newmidi->timecode);
	}
	return newmidi;
}

// Extracts an MTrk (MIDI Track) from a block.
//  Returns: a new malloc'd mtrk struct containing a proper, repaired, mtrk.
struct mtrk *extract_mtrk(unsigned char *buffer)
{
	struct mtrk* newtrack = NULL;
	unsigned int ptr=0;

// This is the "end of track" command
	unsigned char end_of_track[]={0x00,0xFF,0x2F,0x00};

	if (strncmp((char *)buffer,"MTrk",4) != 0)
	{
		printf(" Expected MTrk for track, but couldn't find it!\n");
		return NULL;
	} else {
//		printf(" Found MTrk tag for MIDI track\n");

		newtrack = malloc(sizeof(struct mtrk));
		newtrack->next = NULL;

		newtrack->size = ((unsigned int)buffer[4] * 16777216) +
			((unsigned int)buffer[5] * 65536) +
			((unsigned int)buffer[6] * 256) +
			(unsigned int)buffer[7];
	
		printf(" MTrk is %d bytes long\n",newtrack->size);

		// ipointer should now point to
		//  an "end of track" marker
		if (memcmp(&buffer[newtrack->size+0x04],end_of_track,4) != 0) {
			if (memcmp(&buffer[newtrack->size+0x05],&end_of_track[1],3) != 0) {
				printf("  Expected end-of-track but couldn't find it!\n  Instead I got: 0x%02x 0x%02x 0x%02x 0x%02x\n",buffer[newtrack->size+0x04],buffer[newtrack->size+0x04+1],buffer[newtrack->size+0x04+2],buffer[newtrack->size+0x04+3]);
				printf("  Sometimes this indicates the song has been overwritten.  I'll try to backtrack.\n");
				for (ptr = newtrack->size+0x04; ptr > 8; ptr--)
				{
					if (strncmp((char *)&buffer[ptr],"MThd",4) == 0)
					{
						printf("  Yes, looks like song was saved over.  Terminating and splitting here (%u -> %u).\n",newtrack->size,ptr);
						newtrack->size = ptr-0x08;
						newtrack->data = malloc(ptr + 0x04);
						memcpy(newtrack->data,&buffer[8],ptr);
						memcpy(&newtrack->data[ptr],end_of_track,4);
						newtrack->size += 0x04;
						newtrack->extratrunc = 1;
						break;
					}
				}
				if (ptr == 8)
				{
					printf("  Nope, file was simply damaged.  I'll just try to append a terminator and hope for the best.\n");
					newtrack->data = malloc(newtrack->size + 0x04);
					memcpy(newtrack->data,&buffer[8],newtrack->size);
					memcpy(&newtrack->data[newtrack->size],end_of_track,4);
					newtrack->size += 0x04;
					newtrack->extratrunc = 1;
				}
/*				printf("  I'll try rewinding the stream to look for it.\n");
				for (ptr = newtrack->size+0x04; ptr > 0; ptr--)
				{
					if (memcmp(&buffer[1+ptr],&end_of_track[1],3) == 0) {
						printf("  Got it.  MTrk length should be %d instead\n",ptr+3);
						if (strncmp((char *)&buffer[4+ptr],"MTrk",4) == 0) {
							printf("   This is a good match, it's followed immediately by MTrk.\n");
						}
					break;
					}
				}
				printf("  I'll try advancing the stream to look for it.\n");
				for (ptr = newtrack->size+0x05; ; ptr++)
				{
					if (memcmp(&buffer[1+ptr],&end_of_track[1],3) == 0) {
						printf("  Got it.  MTrk length should be %d instead\n",ptr+3);
						break;
					} else if (strncmp((char *)&buffer[ptr],"MTrk",4) == 0 || strncmp((char *)&buffer[ptr],"MThd",4) == 0) {
						printf("  Bummer, I hit another track (or song).\n  I'll just terminate this one correctly and hope for the best.\n");
						break;
					}
				} */
			} else {
				printf("  Got partial (0xff2f00) end-of-track, it's unusual but OK\n");
				newtrack->data = malloc(newtrack->size);
				memcpy(newtrack->data,&buffer[8],newtrack->size);
				newtrack->extratrunc = 0;
			}
		} else {
			printf("  Got complete end-of-track, seems consistent enough...\n");
			newtrack->data = malloc(newtrack->size);
			memcpy(newtrack->data,&buffer[8],newtrack->size);
			newtrack->extratrunc = 0;
		}
	}
	return newtrack;
}

// Function for "smart extract" of series of MTrks.
//  Given a mthd struct, fills the linked list.
unsigned int smart_extract(struct mthd* midi, unsigned char *buffer,unsigned long eof_point, unsigned long max_distance, unsigned long offset)
{
	long int i=0,j;
	unsigned short int curtrack=0;
	unsigned char in_mthd = 1,lost_sync=0;

	char output_filename[1000];

	struct mtrk *newtrack, *track=NULL;

	while (in_mthd)
	{
		if (strncmp((char *)&buffer[i],"MThd",4) == 0)
		{
			printf(" Collision with another MIDI, we came up short in tracks (expected %hu, got %hu).\n",midi->numtracks,curtrack);
			midi->numtracks = curtrack;
			midi->is_damaged = 1;
			in_mthd = 0;
		} else if (strncmp((char *)&buffer[i],"MTrk",4) != 0)
		{
			printf(" Missing MTrk tag for track %hd, this indicates a damaged MIDI file.\n  Starting recovery search.\n",curtrack);
			midi->is_damaged = 1;

			lost_sync = 1;
			// Recovery search.  Look from here to end of file, max distance, and don't look into other MIDIs : )
			for (j=1; j<eof_point && j<max_distance && strncmp((char *)&buffer[i+j],"MThd",4) != 0; j++)
			{
				if (strncmp((char *)&buffer[i+j],"MTrk",4) == 0)
				{
					printf(" Found an MTrk tag at point %ld.  %ld bytes were lost, but at least we regained sync.\n",i+j,j);
					i += j;
					lost_sync=0;
					break;
				}
			}
			if (lost_sync)
			{
				printf(" Recovery search exceeded EOF or max_distance, or entered another MIDI header.  Truncating MIDI file here.\n");
				midi->numtracks = curtrack;
				in_mthd = 0;
			}
		} else {
			printf(" Found MTrk for track %hd\n",curtrack);
			newtrack = extract_mtrk(&buffer[i]);

			i+=(newtrack->size+0x08);
			if (newtrack->extratrunc) i -= 0x04;
			if (midi->track0 == NULL)
			{
				midi->track0 = newtrack;
			} else {
				track->next=newtrack;
			}
			track=newtrack;

			curtrack ++;
			if (curtrack >= midi->numtracks)
			{
				in_mthd = 0;
			}
		}
	}

	if (midi->is_generated == 1)
		sprintf(output_filename,"%s/mc-%08ld-ORPH.mid",out_dir,offset);
	else if (midi->is_damaged == 0)
		sprintf(output_filename,"%s/mc-%08ld-OK.mid",out_dir,offset);
	else
		sprintf(output_filename,"%s/mc-%08ld-BAD.mid",out_dir,offset);
	write_midi(midi,output_filename);

	return i;
}

int main(int argc, char *argv[])
{
// C89 requires defines at top of file
	FILE *binfile;
	long filesize,i=0,j;
	short miditype,numtracks,curtrack;
	int ipointer;
	unsigned char *buffer;

	struct mthd *midi;
	struct mtrk *track, *newtrack;

// Some flags for recovery features
	int in_mthd=0;

	printf("*************************************************************\n******** MIDI CARVER - Greg Kennedy 2010\n");
	if (argc != 2)
	{
		fprintf(stderr,"Usage: %s <binfile.img>\n",argv[0]);
		return 0;
	}

// does a mkdir so we have somewhere to dump output files
	strcpy(out_dir,dirname(argv[1]));
	strcat(out_dir,"/mcut-out/");
	mkdir(out_dir,S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);

// Open the binary blob for reading.
	binfile = fopen(argv[1],"rb");
	if (binfile == NULL)
	{
		fprintf(stderr,"Could not open %s!\n",argv[1]);
		return -1;
	}

	printf("INFO: Opened %s for reading\n",argv[1]);
	fseek(binfile,0,SEEK_END);
	filesize = ftell(binfile);
	printf("INFO: File is %ld bytes long\n",filesize);
	buffer = malloc(filesize);

	printf("INFO: Reading entire file into RAM...");
	fseek(binfile,0,SEEK_SET);
	fread(buffer,filesize,1,binfile);
	printf("done!\n");
	fclose(binfile);

	// Start looping through the buffer.
	while (i < filesize)
	{
// MTrk outside of an MThd.
//   This is an orphan MTrk, which would need a new generic MThd to contain it.
		if (strncmp(&buffer[i],"MTrk",4) == 0)
		{
			printf("**********************\nFound an orphan MIDI Track at %ld, source is maybe fragmented. : (\n", i);
			printf(" Generating a default type 1 MThd.\n");
			midi=malloc(sizeof(struct mthd));
			midi->track0=NULL;
			midi->miditype=1;
			midi->timecode=120;
			midi->numtracks = 0;
			midi->is_damaged=1;
			midi->is_generated=1;
			printf(" Counting MTrks from here to next MThd...");
			
			for (j=i; j<filesize-3 && strncmp(&buffer[j],"MThd",4) != 0; j++)
			{
				if (strncmp(&buffer[j],"MTrk",4) == 0)
					midi->numtracks ++;
			}
			printf(" found %hd MTrk tags.  Beginning extraction.\n",midi->numtracks);
			i += smart_extract(midi,&buffer[i],filesize-i,32768,i);
		} else if (strncmp(&buffer[i],"MThd",4) == 0)
		{
			printf("*********************************\nFound a MIDI Header starting at %ld\n",i);
// Extract the header and advance the buffer pointer.
			midi = extract_mthd(&buffer[i]);
			i += 14;

			i += smart_extract(midi,&buffer[i],filesize-i,32768,i-14);
		} else {
			++i;
		}
	}
	free(buffer);
	return 0;
}
