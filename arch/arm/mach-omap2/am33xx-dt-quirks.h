#ifndef AM33XX_DT_QUIRKS_H
#define AM33XX_DT_QUIRKS_H

#if IS_ENABLED(OF_DYNAMIC)
extern void __init am33xx_dt_quirk(void);
#else
#define am33xx_dt_quirk	NULL
#endif

#endif
