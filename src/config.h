/* appearance */
static const unsigned int borderpx         = 2;
static const float bordercolor[]           = {0.5, 0.5, 0.5, 1.0};
static const float focuscolor[]            = {1.0, 0.0, 0.0, 1.0};
/* To conform the xdg-protocol, set the alpha to zero to restore the old behavior */
static const float fullscreen_bg[]         = {0.1, 0.1, 0.1, 1.0};

/* tagging - tagcount must be no greater than 31 */
static const int tagcount = 9;

/* monitors */
static const struct MonitorRule monrules[] = {
	/* name       mfact nmaster scale rotate/reflect                x    y */
	{ NULL,       0.55, 1,      1,    WL_OUTPUT_TRANSFORM_NORMAL,   +1,  -1 },
	{ "eDP-1",    0.5,  1,      2,    WL_OUTPUT_TRANSFORM_NORMAL,   -1,  -1 },
};
