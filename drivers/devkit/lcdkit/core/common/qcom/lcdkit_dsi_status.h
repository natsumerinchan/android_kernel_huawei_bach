
#ifndef __LCDKIT_DSI_STATUS_H__
#define __LCDKIT_DSI_STATUS_H__

void mdss_dsi_status_check_ctl(struct msm_fb_data_type *mfd, int sheduled);

int fb_notifier_callback(struct notifier_block* self, unsigned long event, void* data);

#endif
