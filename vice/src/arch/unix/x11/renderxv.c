/*
 * renderxv.c - XVideo rendering.
 *
 * Written by
 *  Dag Lem <resid@nimrod.no>
 *
 * This file is part of VICE, the Versatile Commodore Emulator.
 * See README for copyright notice.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program; if not, write to the Free Software
 *  Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA
 *  02111-1307  USA.
 *
 */


/* The PAL Y/C and PAL Composite emulation is based on work by John
   Selck <graham@cruise.de>. The Xv probing and allocation code is
   loosely based on testxv.c (by Andr� Werthmann) and VideoLAN. */

#include "vice.h"

#ifdef HAVE_XVIDEO

#include "renderxv.h"

#include "video.h"
#include "video/video-resources.h"

#include <stdio.h>
#include <string.h>

/* YUV formats in preferred order. */
fourcc_t fourcc_list[] = {
  /* YUV 4:2:2 formats: */
  { FOURCC_UYVY },
  { FOURCC_YUY2 },
  { FOURCC_YVYU },
  /* YUV 4:1:1 formats: */
  { FOURCC_YV12 },
  { FOURCC_I420 },
  { FOURCC_IYUV }, /* IYUV is a duplicate of I420. */
};

int find_yuv_port(Display* display, XvPortID* port, fourcc_t* format)
{
  int i, j, k;

  /* XvQueryExtension */
  unsigned int version, release, request_base, event_base, error_base;

  /* XvQueryAdaptors */
  int num_adaptors;
  XvAdaptorInfo* adaptor_info = NULL;
  XvPortID port_id;

  /* XvListImageFormats */
  int num_formats;
  XvImageFormatValues* format_list = NULL;

  switch (XvQueryExtension(display, &version, &release,
			   &request_base, &event_base, &error_base))
  {
  case Success:
    break;
  case XvBadExtension:
    printf("XvQueryExtension returned XvBadExtension.\n");
    return 0;
  case XvBadAlloc:
    printf("XvQueryExtension returned XvBadAlloc.\n");
    return 0;
  default:
    printf("XvQueryExtension returned unknown error.\n");
    return 0;
  }

  switch (XvQueryAdaptors(display, DefaultRootWindow(display),
			  &num_adaptors, &adaptor_info))
  {
  case Success:
    break;
  case XvBadExtension:
    printf("XvQueryAdaptors returned XvBadExtension.\n");
    return 0;
  case XvBadAlloc:
    printf("XvQueryAdaptors returned XvBadAlloc.\n");
    return 0;
  default:
    printf("XvQueryAdaptors returned unknown error.\n");
    return 0;
  }

  /* Find YUV capable adaptor. */
  for (i = 0; i < num_adaptors; i++) {
    if (!(adaptor_info[i].type & XvInputMask
	  && adaptor_info[i].type & XvImageMask))
    {
      continue;
    }

    format_list = XvListImageFormats(display, adaptor_info[i].base_id,
				     &num_formats);

    for (j = 0; j < sizeof(fourcc_list)/sizeof(*fourcc_list); j++) {
      if (format->id && fourcc_list[j].id != format->id) {
	continue;
      }
      for (k = 0; k < num_formats; k++) {
	if (format_list[k].id != fourcc_list[j].id) {
	  continue;
	}

	for (port_id = adaptor_info[i].base_id;
	     port_id < adaptor_info[i].base_id + adaptor_info[i].num_ports;
	     port_id++)
	  {
	    if (XvGrabPort(display, port_id, CurrentTime) != Success) {
	      continue;
	    }
	    *port = port_id;
	    *format = fourcc_list[j];
	    XFree(format_list);
	    XvFreeAdaptorInfo(adaptor_info);
	    return 1;
	  }
      }
    }

    XFree(format_list);
  }

  XvFreeAdaptorInfo(adaptor_info);
  printf("No suitable Xv YUV adaptor/port available.\n");
  return 0;
}


XvImage* create_yuv_image(Display* display, XvPortID port, fourcc_t format,
			  int width, int height, XShmSegmentInfo* shminfo)
{
  XvImage* image;

  if (shminfo) {
    if (!(image = XvShmCreateImage(display, port, format.id, NULL,
				   width, height, shminfo)))
    {
      printf("Unable to create shm XvImage\n");
      return NULL;
    }
      
    if ((shminfo->shmid = shmget(IPC_PRIVATE, image->data_size, IPC_CREAT | 0777)) == -1)
    {
      printf("Unable to allocate shared memory\n");
      XFree(image);
      return NULL;
    }
    if (!(shminfo->shmaddr = shmat(shminfo->shmid, 0, 0))) {
      printf("Unable to attach shared memory\n");
      XFree(image);
      shmctl(shminfo->shmid, IPC_RMID, 0);
      return NULL;
    }
    shminfo->readOnly = False;

    image->data = shminfo->shmaddr;

    if (!XShmAttach(display, shminfo)) {
      printf("XShmAttach failed\n");
      XFree(image);
      shmctl(shminfo->shmid, IPC_RMID, 0);
      shmdt(shminfo->shmaddr);
      return NULL;
    }

    /* Send image to X server. This instruction is required, since having
     * built a Shm XImage and not using it causes an error on XCloseDisplay. */
    XSync(display, False);

    /* Mark the segment to be automatically removed when the last
       attachment is broken (i.e. on shmdt or process exit). */
    shmctl(shminfo->shmid, IPC_RMID, 0);
  }
  else {
    if (!(image = XvCreateImage(display, port, format.id, NULL,
				width, height)))
    {
      printf("Unable to create XvImage\n");
      return NULL;
    }
    image->data = malloc(image->data_size);
  }

  return image;
}


void destroy_yuv_image(Display* display, XvImage* image,
		       XShmSegmentInfo* shminfo)
{
  if (shminfo) {
    XShmDetach(display, shminfo);
    XFree(image);
    shmdt(shminfo->shmaddr);
  }
  else {
    XFree(image);
  }
}


void display_yuv_image(Display* display, XvPortID port, Drawable d, GC gc,
		       XvImage* image,
		       XShmSegmentInfo* shminfo,
		       int src_x, int src_y,
		       unsigned int src_w, unsigned int src_h,
		       unsigned int dest_w, unsigned int dest_h)
{
  int dest_x = 0, dest_y = 0;

  /* Keep aspect ratio of src image. */
  if (dest_w*src_h < src_w*dest_h) {
    dest_y = dest_h;
    dest_h = dest_w*src_h/src_w;
    dest_y = (dest_y - dest_h)/2;
  }
  else {
    dest_x = dest_w;
    dest_w = dest_h*src_w/src_h;
    dest_x = (dest_x - dest_w)/2;
  }

  if (shminfo) {
    XvShmPutImage(display, port, d, gc, image,
		  src_x, src_y, src_w, src_h,
		  dest_x, dest_y, dest_w, dest_h, False);
  }
  else {
    XvPutImage(display, port, d, gc, image,
	       src_x, src_y, src_w, src_h,
	       dest_x, dest_y, dest_w, dest_h);
  }
}


/* Extract YUV components. */
inline static unsigned int Y(unsigned int YUV)
{
  return YUV >> 16;
}

inline static unsigned int U(unsigned int YUV)
{
  return (YUV >> 8) & 0xff;
}

inline static unsigned int V(unsigned int YUV)
{
  return YUV & 0xff;
}


typedef struct
{
  unsigned int Y0, Y1, U, V;
} YUV_avg;

YUV_avg yuv_lines[2][1024];


/* Render packed YUV 4:2:2 formats. */
void render_4_2_2(XvImage* image,
		  int shift_y0, int shift_u, int shift_v, int shift_y1,
		  unsigned char* src,
		  int src_pitch,
		  unsigned int* src_color,
		  int src_x, int src_y,
		  unsigned int src_w, unsigned int src_h,
		  int dest_x, int dest_y)
{
  int x, y;
  unsigned int* dest = (unsigned int*)(image->data + image->offsets[0]);
  int dest_pitch = image->pitches[0]/4;

  /* Normalize to 2x1 blocks. */
  if (dest_x & 1) {
    dest_x--;
    src_x--;
    src_w++;
  }
  if (src_w & 1) {
    src_w++;
  }

  /* Add start offsets. */
  dest += dest_pitch*dest_y + (dest_x >> 1);
  src += src_pitch*src_y + src_x;

  /* Render 2x1 blocks, YUV 4:2:2 */
  for (y = 0; y < src_h; y++) {
    for (x = 0; x < src_w; x += 2) {
      unsigned int YUV0 = src_color[*src++];
      unsigned int YUV1 = src_color[*src++];
      *dest++ =
	(Y(YUV0) << shift_y0)
	| (((U(YUV0) + U(YUV1)) >> 1) << shift_u)
	| (((V(YUV0) + V(YUV1)) >> 1) << shift_v)
	| (Y(YUV1) << shift_y1);
    }
    src += src_pitch - src_w;
    dest += dest_pitch - (src_w >> 1);
  }
}


/* Render packed YUV 4:2:2 formats, double size. */
void render2x_4_2_2(XvImage* image,
		    int shift_y0, int shift_u, int shift_v, int shift_y1,
		    unsigned char* src,
		    int src_pitch,
		    unsigned int* src_color,
		    int src_x, int src_y,
		    unsigned int src_w, unsigned int src_h,
		    int dest_x, int dest_y,
		    int double_scan, int pal_scanline_shade)
{
  int x, y;
  unsigned int* dest = (unsigned int*)(image->data + image->offsets[0]);
  int dest_pitch = image->pitches[0]/4;

  /* No need to normalize to 2x1 blocks because of size doubling. */

  /* Add start offsets. */
  dest += (dest_pitch << 1)*dest_y + dest_x;
  src += src_pitch*src_y + src_x;

  /* Render 2x1 blocks, YUV 4:2:2 */
  for (y = 0; y < src_h; y++) {
    for (x = 0; x < src_w; x++) {
      unsigned int YUV = src_color[*src++];
      unsigned int Y0 = Y(YUV);
      unsigned int color =
	(U(YUV) << shift_u)
	| (V(YUV) << shift_v);
      unsigned int pixel2 =
	(Y0 << shift_y0)
	| color
	| (Y0 << shift_y1);
      *dest = pixel2;
      if (!double_scan) {
	/* Set scanline shade intensity. */
	Y0 = Y0*pal_scanline_shade >> 10;
	pixel2 =
	  (Y0 << shift_y0)
	  | color
	  | (Y0 << shift_y1);
      }
      *(dest + dest_pitch) = pixel2;
      dest++;
    }
    src += src_pitch - src_w;
    dest += (dest_pitch << 1) - src_w;
  }
}


/* Render packed YUV 4:2:2 formats - PAL emulation. */
void render_4_2_2_pal(XvImage* image,
		      int shift_y0, int shift_u, int shift_v, int shift_y1,
		      unsigned char* src,
		      int src_pitch,
		      unsigned int* src_color,
		      int src_x, int src_y,
		      unsigned int src_w, unsigned int src_h,
		      int dest_x, int dest_y,
		      int pal_mode)
{
  int x, y;
  unsigned int YUVm1, YUV0, YUV1, YUV2;
  int lineno = 0;
  YUV_avg* linepre;
  YUV_avg* line;
  unsigned int* dest = (unsigned int*)(image->data + image->offsets[0]);
  int dest_pitch = image->pitches[0]/4;

  /* Normalize to 2x1 blocks. */
  if (dest_x & 1) {
    dest_x--;
    src_x--;
    src_w++;
  }
  if (src_w & 1) {
    src_w++;
  }

  /* Enlarge the rendered area to ensure that neighboring pixels are
     correctly averaged. */
  if (dest_x > 0) {
    dest_x -= 2;
    src_x -= 2;
    src_w += 2;
  }
  if (dest_x + src_w < image->width) {
    src_w += 2;
  }
  if (dest_y + src_h < image->height) {
    src_h++;
  }

  /* Add start offsets. */
  dest += dest_pitch*dest_y + (dest_x >> 1);
  src += src_pitch*src_y + src_x;

  if (dest_y > 0) {
    /* Store previous line. */
    linepre = yuv_lines[lineno];
    line = linepre;
    src -= src_pitch;

    /* Read first two pixels. */
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
    }
    else {
      YUVm1 = src_color[*src];
    }
    YUV0 = src_color[*src];
    for (x = 0; x < src_w - 2; x += 2) {
      /* Read next two pixels. */
      YUV1 = src_color[*++src];
      YUV2 = src_color[*++src];

      line->U = U(YUVm1) + U(YUV0) + U(YUV1) + U(YUV2);
      line->V = V(YUVm1) + V(YUV0) + V(YUV1) + V(YUV2);

      /* Prepare to read next two pixels. */
      line++;
      YUVm1 = YUV1;
      YUV0 = YUV2;
    }
    /* Read last two pixels. */
    YUV1 = src_color[*++src];
    if (dest_x + src_w < image->width) {
      YUV2 = src_color[*++src];
    }
    else {
      YUV2 = src_color[*src++];
    }
    line->U = U(YUVm1) + U(YUV0) + U(YUV1) + U(YUV2);
    line->V = V(YUVm1) + V(YUV0) + V(YUV1) + V(YUV2);

    src += src_pitch - src_w;
  }
  else {
    /* Trick main loop into averaging first line with itself. */
    linepre = yuv_lines[lineno ^ 1];
  }

  for (y = 0; y < src_h; y++) {
    /* Store current line. */
    lineno ^= 1;
    line = yuv_lines[lineno];

    /* Read first two pixels. */
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
    }
    else {
      YUVm1 = src_color[*src];
    }
    YUV0 = src_color[*src];
    for (x = 0; x < src_w - 2; x += 2) {
      /* Read next two pixels. */
      YUV1 = src_color[*++src];
      YUV2 = src_color[*++src];

      if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
	line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
	line->Y1 = (Y(YUV0) + (Y(YUV1) << 1) + Y(YUV2)) >> 2;
      }
      else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
	line->Y0 = Y(YUV0);
	line->Y1 = Y(YUV1);
      }
      line->U = U(YUVm1) + U(YUV0) + U(YUV1) + U(YUV2);
      line->V = V(YUVm1) + V(YUV0) + V(YUV1) + V(YUV2);

      /* Prepare to read next two pixels. */
      line++;
      YUVm1 = YUV1;
      YUV0 = YUV2;
    }
    /* Read last two pixels. */
    YUV1 = src_color[*++src];
    if (dest_x + src_w < image->width) {
      YUV2 = src_color[*++src];
    }
    else {
      YUV2 = src_color[*src++];
    }
    if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
      line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
      line->Y1 = (Y(YUV0) + (Y(YUV1) << 1) + Y(YUV2)) >> 2;
    }
    else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
      line->Y0 = Y(YUV0);
      line->Y1 = Y(YUV1);
    }
    line->U = U(YUVm1) + U(YUV0) + U(YUV1) + U(YUV2);
    line->V = V(YUVm1) + V(YUV0) + V(YUV1) + V(YUV2);
    src += src_pitch - src_w;

    line = yuv_lines[lineno];

    /* Render 2x1 blocks, YUV 4:2:2 */
    for (x = 0; x < src_w; x += 2) {
      *dest++ =
	(line->Y0 << shift_y0)
	| (((line->U + linepre->U) >> 3) << shift_u)
	| (((line->V + linepre->V) >> 3) << shift_v)
	| (line->Y1 << shift_y1);
      line++;
      linepre++;
    }
    dest += dest_pitch - (src_w >> 1);

    linepre = yuv_lines[lineno];
  }
}


/* Render packed YUV 4:2:2 formats - PAL emulation, double size. */
void render2x_4_2_2_pal(XvImage* image,
			int shift_y0, int shift_u, int shift_v, int shift_y1,
			unsigned char* src,
			int src_pitch,
			unsigned int* src_color,
			int src_x, int src_y,
			unsigned int src_w, unsigned int src_h,
			int dest_x, int dest_y,
			int pal_mode,
			int double_scan, int pal_scanline_shade)
{
  int x, y;
  unsigned int YUVm2, YUVm1, YUV0, YUV1;
  int lineno = 0;
  YUV_avg* linepre;
  YUV_avg* line;
  unsigned int* dest = (unsigned int*)(image->data + image->offsets[0]);
  int dest_pitch = image->pitches[0]/4;

  /* No need to normalize to 2x1 blocks because of size doubling. */

  /* Enlarge the rendered area to ensure that neighboring pixels are
     correctly averaged. */
  dest_x--;
  src_x--;
  src_w++;
  if (dest_x < 0) {
    src_x -= dest_x;
    src_w += dest_x;
    dest_x = 0;
  }
  src_w += 2;
  if (dest_x + src_w > image->width >> 1) {
    src_w = (image->width >> 1) - dest_x;
  }
  if (dest_y + src_h < image->height >> 1) {
    src_h++;
  }

  /* Add start offsets. */
  dest += (dest_pitch << 1)*dest_y + dest_x;
  src += src_pitch*src_y + src_x;

  if (dest_y > 0) {
    /* Store previous line. */
    linepre = yuv_lines[lineno];
    line = linepre;
    src -= src_pitch;

    /* Read first three pixels. */
    YUVm2 = YUVm1 = YUV0 = src_color[*src];
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
      if (dest_x > 1) {
	YUVm2 = src_color[*(src - 2)];
      }
    }
    for (x = 0; x < src_w - 1; x++) {
      /* Read next pixel. */
      YUV1 = src_color[*++src];

      line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
      line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

      /* Prepare to read next pixel. */
      line++;
      YUVm2 = YUVm1;
      YUVm1 = YUV0;
      YUV0 = YUV1;
    }
    /* Read last pixel. */
    if (dest_x + src_w < image->width >> 1) {
      YUV1 = src_color[*++src];
    }
    else {
      YUV1 = src_color[*src++];
    }
    line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
    line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

    src += src_pitch - src_w;
  }
  else {
    /* Trick main loop into averaging first line with itself. */
    linepre = yuv_lines[lineno ^ 1];
  }

  for (y = 0; y < src_h; y++) {
    /* Store current line. */
    lineno ^= 1;
    line = yuv_lines[lineno];

    /* Read first three pixels. */
    YUVm2 = YUVm1 = YUV0 = src_color[*src];
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
      if (dest_x > 1) {
	YUVm2 = src_color[*(src - 2)];
      }
    }
    for (x = 0; x < src_w - 1; x++) {
      /* Read next pixel. */
      YUV1 = src_color[*++src];

      if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
	line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
      }
      else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
	line->Y0 = Y(YUV0);
      }
      line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
      line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

      /* Prepare to read next pixel. */
      line++;
      YUVm2 = YUVm1;
      YUVm1 = YUV0;
      YUV0 = YUV1;
    }
    /* Read last pixel. */
    if (dest_x + src_w < image->width >> 1) {
      YUV1 = src_color[*++src];
    }
    else {
      YUV1 = src_color[*src++];
    }
    if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
      line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
    }
    else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
      line->Y0 = Y(YUV0);
    }
    line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
    line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);
    src += src_pitch - src_w;

    line = yuv_lines[lineno];

    /* Render 2x1 blocks, YUV 4:2:2 */
    for (x = 0; x < src_w; x++) {
      unsigned int color = 
	(((line->U + linepre->U) >> 3) << shift_u)
	| (((line->V + linepre->V) >> 3) << shift_v);
      unsigned int pixel2 =
	(line->Y0 << shift_y0)
	| color
	| (line->Y0 << shift_y1);
      *dest = pixel2;
      if (!double_scan) {
	/* Set scanline shade intensity. */
	unsigned int Y0 = line->Y0*pal_scanline_shade >> 10;
	pixel2 =
	  (Y0 << shift_y0)
	  | color
	  | (Y0 << shift_y1);
      }
      *(dest + dest_pitch) = pixel2;
      dest++;
      line++;
      linepre++;
    }
    dest += (dest_pitch << 1) - src_w;

    linepre = yuv_lines[lineno];
  }
}


/* Render planar YUV 4:1:1 formats. */
void render_4_1_1(XvImage* image,
		  int plane_y, int plane_u, int plane_v,
		  unsigned char* src,
		  int src_pitch,
		  unsigned int* src_color,
		  int src_x, int src_y,
		  unsigned int src_w, unsigned int src_h,
		  int dest_x, int dest_y)
{
  int x, y;
  unsigned char* Yptr = image->data + image->offsets[plane_y];
  unsigned char* Uptr = image->data + image->offsets[plane_u];
  unsigned char* Vptr = image->data + image->offsets[plane_v];
  int Ypitch = image->pitches[plane_y];
  int Upitch = image->pitches[plane_u];
  int Vpitch = image->pitches[plane_v];

  /* Normalize to 2x2 blocks. */
  if (dest_x & 1) {
    dest_x--;
    src_x--;
    src_w++;
  }
  if (src_w & 1) {
    src_w++;
  }
  if (dest_y & 1) {
    dest_y--;
    src_y--;
    src_h++;
  }
  if (src_h & 1) {
    src_h++;
  }

  /* Add start offsets. */
  Yptr += Ypitch*dest_y + dest_x;
  Uptr += (Upitch*dest_y + dest_x) >> 1;
  Vptr += (Vpitch*dest_y + dest_x) >> 1;
  src += src_pitch*src_y + src_x;

  /* Render 2x2 blocks, YUV 4:1:1 */
  for (y = 0; y < src_h; y += 2) {
    for (x = 0; x < src_w; x += 2) {
      unsigned int YUV0 = src_color[*src];
      unsigned int YUV1 = src_color[*(src + 1)];
      unsigned int YUV2 = src_color[*(src + src_pitch)];
      unsigned int YUV3 = src_color[*(src + src_pitch + 1)];
      src += 2;
      *Yptr = Y(YUV0);
      *(Yptr + 1) = Y(YUV1);
      *(Yptr + Ypitch) = Y(YUV2);
      *(Yptr + Ypitch + 1) = Y(YUV3);
      Yptr += 2;
      *Uptr++ = (U(YUV0) + U(YUV1) + U(YUV2) + U(YUV3)) >> 2;
      *Vptr++ = (V(YUV0) + V(YUV1) + V(YUV2) + V(YUV3)) >> 2;
    }
    src += (src_pitch << 1) - src_w;
    Yptr += (Ypitch << 1) - src_w;
    Uptr += Upitch - (src_w >> 1);
    Vptr += Vpitch - (src_w >> 1);
  }
}


/* Render planar YUV 4:1:1 formats, double size. */
void render2x_4_1_1(XvImage* image,
		    int plane_y, int plane_u, int plane_v,
		    unsigned char* src,
		    int src_pitch,
		    unsigned int* src_color,
		    int src_x, int src_y,
		    unsigned int src_w, unsigned int src_h,
		    int dest_x, int dest_y,
		    int double_scan, int pal_scanline_shade)
{
  int x, y;
  unsigned char* Yptr = image->data + image->offsets[plane_y];
  unsigned char* Uptr = image->data + image->offsets[plane_u];
  unsigned char* Vptr = image->data + image->offsets[plane_v];
  int Ypitch = image->pitches[plane_y];
  int Upitch = image->pitches[plane_u];
  int Vpitch = image->pitches[plane_v];

  /* No need to normalize to 2x2 blocks because of size doubling. */

  /* Add start offsets. */
  Yptr += (Ypitch*dest_y + dest_x) << 1;
  Uptr += Upitch*dest_y + dest_x;
  Vptr += Vpitch*dest_y + dest_x;
  src += src_pitch*src_y + src_x;

  /* Render 2x2 blocks, YUV 4:1:1 */
  for (y = 0; y < src_h; y++) {
    for (x = 0; x < src_w; x++) {
      unsigned int YUV = src_color[*src++];
      unsigned int Y0 = Y(YUV);
      *Yptr = Y0;
      *(Yptr + 1) = Y0;
      if (!double_scan) {
	/* Set scanline shade intensity. */
	Y0 = Y0*pal_scanline_shade >> 10;
      }
      *(Yptr + Ypitch) = Y0;
      *(Yptr + Ypitch + 1) = Y0;
      Yptr += 2;
      *Uptr++ = U(YUV);
      *Vptr++ = V(YUV);
    }
    src += src_pitch - src_w;
    Yptr += (Ypitch - src_w) << 1;
    Uptr += Upitch - src_w;
    Vptr += Vpitch - src_w;
  }
}


/* Render planar YUV 4:1:1 formats - PAL emulation. */
void render_4_1_1_pal(XvImage* image,
		      int plane_y, int plane_u, int plane_v,
		      unsigned char* src,
		      int src_pitch,
		      unsigned int* src_color,
		      int src_x, int src_y,
		      unsigned int src_w, unsigned int src_h,
		      int dest_x, int dest_y,
		      int pal_mode)
{
  int x, y;
  unsigned int
    YUVm10, YUV00, YUV10, YUV20,
    YUVm11, YUV01, YUV11, YUV21;
  unsigned char* Yptr = image->data + image->offsets[plane_y];
  unsigned char* Uptr = image->data + image->offsets[plane_u];
  unsigned char* Vptr = image->data + image->offsets[plane_v];
  int Ypitch = image->pitches[plane_y];
  int Upitch = image->pitches[plane_u];
  int Vpitch = image->pitches[plane_v];

  /* Normalize to 2x2 blocks. */
  if (dest_x & 1) {
    dest_x--;
    src_x--;
    src_w++;
  }
  if (src_w & 1) {
    src_w++;
  }
  if (dest_y & 1) {
    dest_y--;
    src_y--;
    src_h++;
  }
  if (src_h & 1) {
    src_h++;
  }

  /* Enlarge the rendered area to ensure that neighboring pixels are
     correctly averaged. */
  if (dest_x > 0) {
    dest_x -= 2;
    src_x -= 2;
    src_w += 2;
  }
  if (dest_x + src_w < image->width) {
    src_w += 2;
  }

  /* Add start offsets. */
  Yptr += Ypitch*dest_y + dest_x;
  Uptr += (Upitch*dest_y + dest_x) >> 1;
  Vptr += (Vpitch*dest_y + dest_x) >> 1;
  src += src_pitch*src_y + src_x;

  /* Render 2x2 blocks, YUV 4:1:1 */
  for (y = 0; y < src_h; y += 2) {
    /* Read first 2x2 block. */
    if (dest_x > 0) {
      YUVm10 = src_color[*(src - 1)];
      YUVm11 = src_color[*(src + src_pitch - 1)];
    }
    else {
      YUVm10 = src_color[*src];
      YUVm11 = src_color[*(src + src_pitch)];
    }
    YUV00 =  src_color[*src];
    YUV01 =  src_color[*(src + src_pitch)];
    for (x = 0; x < src_w - 2; x += 2) {
      /* Read next 2x2 block. */
      YUV10 = src_color[*(src + 1)];
      YUV20 = src_color[*(src + 2)];
      YUV11 = src_color[*(src + src_pitch + 1)];
      YUV21 = src_color[*(src + src_pitch + 2)];
      src += 2;

      if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
	*Yptr =               (Y(YUVm10) + (Y(YUV00) << 1) + Y(YUV10)) >> 2;
	*(Yptr + 1) =          (Y(YUV00) + (Y(YUV10) << 1) + Y(YUV20)) >> 2;
	*(Yptr + Ypitch) =    (Y(YUVm11) + (Y(YUV01) << 1) + Y(YUV11)) >> 2;
	*(Yptr + Ypitch + 1) = (Y(YUV01) + (Y(YUV11) << 1) + Y(YUV21)) >> 2;
      }
      else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
	*Yptr = Y(YUV00);
	*(Yptr + 1) = Y(YUV10);
	*(Yptr + Ypitch) = Y(YUV01);
	*(Yptr + Ypitch + 1) = Y(YUV11);
      }
      Yptr += 2;
      *Uptr++ =
	(U(YUVm10) + U(YUV00) + U(YUV10) + U(YUV20) +
	 U(YUVm11) + U(YUV01) + U(YUV11) + U(YUV21)) >> 3;
      *Vptr++ =
	(V(YUVm10) + V(YUV00) + V(YUV10) + V(YUV20) +
	 V(YUVm11) + V(YUV01) + V(YUV11) + V(YUV21)) >> 3;

      /* Prepare to read next 2x2 block. */
      YUVm10 = YUV10; YUV00 = YUV20;
      YUVm11 = YUV11; YUV01 = YUV21;
    }
    /* Read last 2x2 block. */
    YUV10 = src_color[*(src + 1)];
    YUV11 = src_color[*(src + src_pitch + 1)];
    if (dest_x + src_w < image->width) {
      YUV20 = YUV10;
      YUV21 = YUV11;
    }
    else {
      YUV20 = src_color[*(src + 2)];
      YUV21 = src_color[*(src + src_pitch + 2)];
    }
    src += 2;

    if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
      *Yptr =               (Y(YUVm10) + (Y(YUV00) << 1) + Y(YUV10)) >> 2;
      *(Yptr + 1) =          (Y(YUV00) + (Y(YUV10) << 1) + Y(YUV20)) >> 2;
      *(Yptr + Ypitch) =    (Y(YUVm11) + (Y(YUV01) << 1) + Y(YUV11)) >> 2;
      *(Yptr + Ypitch + 1) = (Y(YUV01) + (Y(YUV11) << 1) + Y(YUV21)) >> 2;
    }
    else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
      *Yptr = Y(YUV00);
      *(Yptr + 1) = Y(YUV10);
      *(Yptr + Ypitch) = Y(YUV01);
      *(Yptr + Ypitch + 1) = Y(YUV11);
    }
    Yptr += 2;
    *Uptr++ =
      (U(YUVm10) + U(YUV00) + U(YUV10) + U(YUV20) +
       U(YUVm11) + U(YUV01) + U(YUV11) + U(YUV21)) >> 3;
    *Vptr++ =
      (V(YUVm10) + V(YUV00) + V(YUV10) + V(YUV20) +
       V(YUVm11) + V(YUV01) + V(YUV11) + V(YUV21)) >> 3;

    src += (src_pitch << 1) - src_w;
    Yptr += (Ypitch << 1) - src_w;
    Uptr += Upitch - (src_w >> 1);
    Vptr += Vpitch - (src_w >> 1);
  }
}


/* Render planar YUV 4:1:1 formats - PAL emulation, double size. */
void render2x_4_1_1_pal(XvImage* image,
			int plane_y, int plane_u, int plane_v,
			unsigned char* src,
			int src_pitch,
			unsigned int* src_color,
			int src_x, int src_y,
			unsigned int src_w, unsigned int src_h,
			int dest_x, int dest_y,
			int pal_mode,
			int double_scan, int pal_scanline_shade)
{
  int x, y;
  unsigned int YUVm2, YUVm1, YUV0, YUV1;
  int lineno = 0;
  YUV_avg* linepre;
  YUV_avg* line;
  unsigned char* Yptr = image->data + image->offsets[plane_y];
  unsigned char* Uptr = image->data + image->offsets[plane_u];
  unsigned char* Vptr = image->data + image->offsets[plane_v];
  int Ypitch = image->pitches[plane_y];
  int Upitch = image->pitches[plane_u];
  int Vpitch = image->pitches[plane_v];

  /* No need to normalize to 2x2 blocks because of size doubling. */

  /* Enlarge the rendered area to ensure that neighboring pixels are
     correctly averaged. */
  dest_x--;
  src_x--;
  src_w++;
  if (dest_x < 0) {
    src_x -= dest_x;
    src_w += dest_x;
    dest_x = 0;
  }
  src_w += 2;
  if (dest_x + src_w > image->width >> 1) {
    src_w = (image->width >> 1) - dest_x;
  }
  if (dest_y + src_h < image->height >> 1) {
    src_h++;
  }

  /* Add start offsets. */
  Yptr += (Ypitch*dest_y + dest_x) << 1;
  Uptr += Upitch*dest_y + dest_x;
  Vptr += Vpitch*dest_y + dest_x;
  src += src_pitch*src_y + src_x;

  if (dest_y > 0) {
    /* Store previous line. */
    linepre = yuv_lines[lineno];
    line = linepre;
    src -= src_pitch;

    /* Read first three pixels. */
    YUVm2 = YUVm1 = YUV0 = src_color[*src];
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
      if (dest_x > 1) {
	YUVm2 = src_color[*(src - 2)];
      }
    }
    for (x = 0; x < src_w - 1; x++) {
      /* Read next pixel. */
      YUV1 = src_color[*++src];

      line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
      line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

      /* Prepare to read next pixel. */
      line++;
      YUVm2 = YUVm1;
      YUVm1 = YUV0;
      YUV0 = YUV1;
    }
    /* Read last pixel. */
    if (dest_x + src_w < image->width >> 1) {
      YUV1 = src_color[*++src];
    }
    else {
      YUV1 = src_color[*src++];
    }
    line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
    line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

    src += src_pitch - src_w;
  }
  else {
    /* Trick main loop into averaging first line with itself. */
    linepre = yuv_lines[lineno ^ 1];
  }

  for (y = 0; y < src_h; y++) {
    /* Store current line. */
    lineno ^= 1;
    line = yuv_lines[lineno];

    /* Read first three pixels. */
    YUVm2 = YUVm1 = YUV0 = src_color[*src];
    if (dest_x > 0) {
      YUVm1 = src_color[*(src - 1)];
      if (dest_x > 1) {
	YUVm2 = src_color[*(src - 2)];
      }
    }
    for (x = 0; x < src_w - 1; x++) {
      /* Read next pixel. */
      YUV1 = src_color[*++src];

      if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
	line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
      }
      else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
	line->Y0 = Y(YUV0);
      }
      line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
      line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);

      /* Prepare to read next pixel. */
      line++;
      YUVm2 = YUVm1;
      YUVm1 = YUV0;
      YUV0 = YUV1;
    }
    /* Read last pixel. */
    if (dest_x + src_w < image->width >> 1) {
      YUV1 = src_color[*++src];
    }
    else {
      YUV1 = src_color[*src++];
    }
    if (pal_mode == VIDEO_RESOURCE_PAL_MODE_BLUR) {
      line->Y0 = (Y(YUVm1) + (Y(YUV0) << 1) + Y(YUV1)) >> 2;
    }
    else { /* pal_mode == VIDEO_RESOURCE_PAL_MODE_SHARP */
      line->Y0 = Y(YUV0);
    }
    line->U = U(YUVm2) + U(YUVm1) + U(YUV0) + U(YUV1);
    line->V = V(YUVm2) + V(YUVm1) + V(YUV0) + V(YUV1);
    src += src_pitch - src_w;

    line = yuv_lines[lineno];

    /* Render 2x1 blocks, YUV 4:1:1 */
    for (x = 0; x < src_w; x++) {
      unsigned int Y0 = line->Y0; 
      *Yptr = Y0;
      *(Yptr + 1) = Y0;
      if (!double_scan) {
	/* Set scanline shade intensity. */
	Y0 = line->Y0*pal_scanline_shade >> 10;
      }
      *(Yptr + Ypitch) = Y0;
      *(Yptr + Ypitch + 1) = Y0;
      Yptr += 2;
      *Uptr++ =
	(line->U + linepre->U) >> 3;
      *Vptr++ =
	(line->V + linepre->V) >> 3;
      line++;
      linepre++;
    }
    Yptr += (Ypitch - src_w) << 1;
    Uptr += Upitch - src_w;
    Vptr += Vpitch - src_w;

    linepre = yuv_lines[lineno];
  }
}


/* Render YUV 4:2:2 and 4:1:1 formats. */
void render_yuv_image(int double_size,
		      int double_scan,
		      int pal_mode,
		      int pal_scanline_shade,
		      fourcc_t format,
		      XvImage* image,
		      unsigned char* src,
		      int src_pitch,
		      unsigned int* src_color,
		      int src_x, int src_y,
		      unsigned int src_w, unsigned int src_h,
		      int dest_x, int dest_y)
{
  int planar;
  int shift_y0, shift_u, shift_v, shift_y1;
  int plane_y, plane_u, plane_v;

  switch (format.id) {
  case FOURCC_UYVY:
    planar = 0;
#ifdef WORDS_BIGENDIAN
    shift_y0 = 16; shift_u = 24; shift_v = 8; shift_y1 = 0;
#else
    shift_y0 = 8; shift_u = 0; shift_v = 16; shift_y1 = 24;
#endif
    break;
  case FOURCC_YUY2:
    planar = 0;
#ifdef WORDS_BIGENDIAN
    shift_y0 = 24; shift_u = 16; shift_v = 0; shift_y1 = 8;
#else
    shift_y0 = 0; shift_u = 8; shift_v = 24; shift_y1 = 16;
#endif
    break;
  case FOURCC_YVYU:
    planar = 0;
#ifdef WORDS_BIGENDIAN
    shift_y0 = 24; shift_u = 0; shift_v = 16; shift_y1 = 8;
#else
    shift_y0 = 0; shift_u = 24; shift_v = 8; shift_y1 = 16;
#endif
    break;
  case FOURCC_YV12:
    planar = 1;
    plane_y = 0; plane_u = 2; plane_v = 1;
    break;
  case FOURCC_I420:
  case FOURCC_IYUV:
    planar = 1;
    plane_y = 0; plane_u = 1; plane_v = 2;
    break;
  default:
    return;
  }

  if (double_size) {
    /* 2x2 */
    if (planar) {
      switch(pal_mode) {
      case VIDEO_RESOURCE_PAL_MODE_FAST:
	render2x_4_1_1(image, plane_y, plane_u, plane_v,
		       src, src_pitch, src_color,
		       src_x, src_y, src_w, src_h, dest_x, dest_y,
		       double_scan, pal_scanline_shade);
	break;
      default:
      case VIDEO_RESOURCE_PAL_MODE_SHARP:
      case VIDEO_RESOURCE_PAL_MODE_BLUR:
	render2x_4_1_1_pal(image, plane_y, plane_u, plane_v,
			   src, src_pitch, src_color,
			   src_x, src_y, src_w, src_h, dest_x, dest_y,
			   pal_mode, double_scan, pal_scanline_shade);
	break;
      }
    }
    else {
      switch(pal_mode) {
      case VIDEO_RESOURCE_PAL_MODE_FAST:
	render2x_4_2_2(image, shift_y0, shift_u, shift_v, shift_y1,
		       src, src_pitch, src_color,
		       src_x, src_y, src_w, src_h, dest_x, dest_y,
		       double_scan, pal_scanline_shade);
	break;
      default:
      case VIDEO_RESOURCE_PAL_MODE_SHARP:
      case VIDEO_RESOURCE_PAL_MODE_BLUR:
	render2x_4_2_2_pal(image, shift_y0, shift_u, shift_v, shift_y1,
			  src, src_pitch, src_color,
			  src_x, src_y, src_w, src_h, dest_x, dest_y,
			  pal_mode, double_scan, pal_scanline_shade);
	break;
      }
    }
  }
  else {
    /* 1x1 */
    if (planar) {
      switch(pal_mode) {
      case VIDEO_RESOURCE_PAL_MODE_FAST:
	render_4_1_1(image, plane_y, plane_u, plane_v,
		     src, src_pitch, src_color,
		     src_x, src_y, src_w, src_h, dest_x, dest_y);
	break;
      default:
      case VIDEO_RESOURCE_PAL_MODE_SHARP:
      case VIDEO_RESOURCE_PAL_MODE_BLUR:
	render_4_1_1_pal(image, plane_y, plane_u, plane_v,
			 src, src_pitch, src_color,
			 src_x, src_y, src_w, src_h, dest_x, dest_y,
			 pal_mode);
	break;
      }
    }
    else {
      switch(pal_mode) {
      case VIDEO_RESOURCE_PAL_MODE_FAST:
	render_4_2_2(image, shift_y0, shift_u, shift_v, shift_y1,
		     src, src_pitch, src_color,
		     src_x, src_y, src_w, src_h, dest_x, dest_y);
	break;
      default:
      case VIDEO_RESOURCE_PAL_MODE_SHARP:
      case VIDEO_RESOURCE_PAL_MODE_BLUR:
	render_4_2_2_pal(image, shift_y0, shift_u, shift_v, shift_y1,
			 src, src_pitch, src_color,
			 src_x, src_y, src_w, src_h, dest_x, dest_y,
			 pal_mode);
	break;
      }
    }
  }
}

#endif /* HAVE_XVIDEO */
