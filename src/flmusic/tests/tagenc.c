/* tagenc IN.wav OUT.ogg title artist album track -- encode WAV to Ogg Vorbis with tags
 * (a test helper: gst-launch cannot set tags). SPDX-License-Identifier: MIT */
#include <gst/gst.h>
#include <stdio.h>
#include <stdlib.h>

int main(int argc, char **argv)
{
	if (argc != 7) {
		fprintf(stderr, "usage: tagenc IN.wav OUT.ogg title artist album track\n");
		return 2;
	}
	gst_init(NULL, NULL);
	char desc[2048];
	snprintf(desc, sizeof desc,
	         "filesrc location=\"%s\" ! wavparse ! audioconvert ! vorbisenc name=enc quality=0.3 ! oggmux ! "
	         "filesink location=\"%s\"", argv[1], argv[2]);
	GError *err = NULL;
	GstElement *p = gst_parse_launch(desc, &err);
	if (!p) {
		fprintf(stderr, "%s\n", err->message);
		return 1;
	}
	GstElement *enc = gst_bin_get_by_name(GST_BIN(p), "enc");
	gst_tag_setter_add_tags(GST_TAG_SETTER(enc), GST_TAG_MERGE_REPLACE, GST_TAG_TITLE, argv[3], GST_TAG_ARTIST,
	                        argv[4], GST_TAG_ALBUM_ARTIST, argv[4], GST_TAG_ALBUM, argv[5], GST_TAG_TRACK_NUMBER,
	                        atoi(argv[6]), NULL);
	gst_element_set_state(p, GST_STATE_PLAYING);
	GstBus *bus = gst_element_get_bus(p);
	GstMessage *m = gst_bus_timed_pop_filtered(bus, GST_CLOCK_TIME_NONE, GST_MESSAGE_EOS | GST_MESSAGE_ERROR);
	int rc = GST_MESSAGE_TYPE(m) == GST_MESSAGE_ERROR;
	gst_message_unref(m);
	gst_element_set_state(p, GST_STATE_NULL);
	return rc;
}
