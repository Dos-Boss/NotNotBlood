
#ifdef RENDERTYPEWIN
# include "winlayer.h"
#else
# include "sdlayer.h"
#endif

#undef STARTUP_SETUP_WINDOW

#ifdef STARTUP_WINDOW
//#if defined _WIN32 || defined EDUKE32_OSX || defined HAVE_GTK2
#if (defined _WIN32 || defined EDUKE32_OSX || defined HAVE_GTK2) && !defined NORENDER
# define STARTUP_SETUP_WINDOW
#endif
#endif

#undef WM_MSGBOX_WINDOW
//#if defined _WIN32 || defined EDUKE32_OSX || defined HAVE_GTK2 || (defined RENDERTYPESDL && SDL_MAJOR_VERSION >= 2)
#if (defined _WIN32 || defined EDUKE32_OSX || defined HAVE_GTK2 || (defined RENDERTYPESDL && SDL_MAJOR_VERSION >= 2)) && !defined NORENDER
# define WM_MSGBOX_WINDOW
#endif
