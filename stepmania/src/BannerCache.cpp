#include "global.h"

#include "BannerCache.h"
#include "Foreach.h"
#include "RageDisplay.h"
#include "RageUtil.h"
#include "RageLog.h"
#include "RageSurface_Load.h"
#include "SongCacheIndex.h"
#include "Sprite.h"
#include "PrefsManager.h"
#include "RageDisplay.h"
#include "RageTexture.h"
#include "RageTextureManager.h"
#include "RageSurface.h"
#include "RageSurfaceUtils.h"
#include "RageSurfaceUtils_Palettize.h"
#include "RageSurfaceUtils_Dither.h"
#include "RageSurfaceUtils_Zoom.h"
#include "SpecialFiles.h"

#include "Banner.h"

/* Neither a global or a file scope static can be used for this because
 * the order of initialization of nonlocal objects is unspecified. */
//const RString BANNER_CACHE_INDEX = SpecialFiles::CACHE_DIR + "banners.cache";
#define BANNER_CACHE_INDEX (SpecialFiles::CACHE_DIR + "banners.cache")

/* Call CacheBanner to cache a banner by path.  If the banner is already
 * cached, it'll be recreated.  This is efficient if the banner hasn't changed,
 * but we still only do this in TidyUpData for songs.
 *
 * Call LoadBanner to load a cached banner into main memory.  This will call
 * CacheBanner only if needed.  This will not do a date/size check; call CacheBanner
 * directly if you need that.
 *
 * Call LoadCachedBanner to load a banner into a texture and retrieve an ID
 * for it.  You can check if the banner was actually preloaded by calling
 * TEXTUREMAN->IsTextureRegistered() on the ID; it might not be if the banner cache
 * is missing or disabled.
 *
 * Note that each cache entries has two hashes.  The cache path is based soley
 * on the pathname; this way, loading the cache doesn't have to do a stat on every
 * banner.  The full hash includes the file size and date, and is used only by
 * CacheBanner to avoid doing extra work.
 */

BannerCache *BANNERCACHE;


static map<RString,RageSurface *> g_BannerPathToImage;
static int g_iDemandRefcount = 0;

RString BannerCache::GetBannerCachePath( RString sBannerPath )
{
	return SongCacheIndex::GetCacheFilePath( "Banners", sBannerPath );
}

/* If in on-demand mode, load all cached banners.  This must be fast, so
 * cache files will not be created if they don't exist; that should be done
 * by CacheBanner or LoadBanner on startup. */
void BannerCache::Demand()
{
	++g_iDemandRefcount;
	if( g_iDemandRefcount > 1 )
		return;
	
	if( PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	FOREACH_CONST_Child( &BannerData, p )
	{
		RString sBannerPath = p->GetName();

		if( g_BannerPathToImage.find(sBannerPath) != g_BannerPathToImage.end() )
			continue; /* already loaded */

		const RString CachePath = GetBannerCachePath(sBannerPath);
		RageSurface *img = RageSurfaceUtils::LoadSurface( CachePath );
		if( img == NULL )
		{
			continue; /* doesn't exist */
		}

		g_BannerPathToImage[sBannerPath] = img;
	}
}

/* Release banners loaded on demand. */
void BannerCache::Undemand()
{
	--g_iDemandRefcount;
	if( g_iDemandRefcount != 0 )
		return;
	
	if( PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	UnloadAllBanners();
}

/* If in a low-res banner mode, load a low-res banner into memory, creating
 * the cache file if necessary.  Unlike CacheBanner(), the original file will
 * not be examined unless the cached banner doesn't exist, so the banner will
 * not be updated if the original file changes, for efficiency. */
void BannerCache::LoadBanner( RString sBannerPath )
{
	if( sBannerPath == "" )
		return; // nothing to do
	if( PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_PRELOAD &&
	    PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	/* Load it. */
	const RString CachePath = GetBannerCachePath(sBannerPath);

	for( int tries = 0; tries < 2; ++tries )
	{
		if( g_BannerPathToImage.find(sBannerPath) != g_BannerPathToImage.end() )
			return; /* already loaded */

		CHECKPOINT_M( ssprintf( "BannerCache::LoadBanner: %s", CachePath.c_str() ) );
		RageSurface *img = RageSurfaceUtils::LoadSurface( CachePath );
		if( img == NULL )
		{
			if( tries == 0 )
			{
				/* The file doesn't exist.  It's possible that the banner cache file is
				 * missing, so try to create it.  Don't do this first, for efficiency. */
				LOG->Trace( "Cached banner load of '%s' ('%s') failed, trying to cache ...", sBannerPath.c_str(), CachePath.c_str() );

				/* Skip the up-to-date check; it failed to load, so it can't be up
				 * to date. */
				CacheBannerInternal( sBannerPath );
				continue;
			}
			else
			{
				LOG->Trace( "Cached banner load of '%s' ('%s') failed", sBannerPath.c_str(), CachePath.c_str() );
				return;
			}
		}

		g_BannerPathToImage[sBannerPath] = img;
	}
}

void BannerCache::OutputStats() const
{
	int iTotalSize = 0;
	FOREACHM_CONST( RString, RageSurface *, g_BannerPathToImage, it )
	{
		const RageSurface *pImage = it->second;
		const int iSize = pImage->pitch * pImage->h;
		iTotalSize += iSize;
	}
	LOG->Info( "%i bytes of banners loaded", iTotalSize );
}

void BannerCache::UnloadAllBanners()
{
	FOREACHM( RString, RageSurface *, g_BannerPathToImage, it )
		delete it->second;

	g_BannerPathToImage.clear();
}

BannerCache::BannerCache()
{
	ReadFromDisk();
}

BannerCache::~BannerCache()
{
	UnloadAllBanners();
}

void BannerCache::ReadFromDisk()
{
	BannerData.ReadFile( BANNER_CACHE_INDEX );	// don't care if this fails
}

struct BannerTexture: public RageTexture
{
	unsigned m_uTexHandle;
	unsigned GetTexHandle() const { return m_uTexHandle; };	// accessed by RageDisplay
	/* This is a reference to a pointer in g_BannerPathToImage. */
	RageSurface *&img;
	int width, height;

	BannerTexture( RageTextureID name, RageSurface *&img_, int width_, int height_ ):
		RageTexture(name), img(img_), width(width_), height(height_)
	{
		Create();
	}

	~BannerTexture()
	{ 
		Destroy();
	}
	
	void Create()
	{
		ASSERT( img );

		/* The image is preprocessed; do as little work as possible. */

		/* The source width is the width of the original file. */
		m_iSourceWidth = width;
		m_iSourceHeight = height;

		/* The image width (within the texture) is always the entire texture. 
		 * Only resize if the max texture size requires it; since these images
		 * are already scaled down, this shouldn't happen often. */
		if( img->w > DISPLAY->GetMaxTextureSize() || 
			img->h > DISPLAY->GetMaxTextureSize() )
		{
			LOG->Warn( "Converted %s at runtime", GetID().filename.c_str() );
			int width = min( img->w, DISPLAY->GetMaxTextureSize() );
			int height = min( img->h, DISPLAY->GetMaxTextureSize() );
			RageSurfaceUtils::Zoom( img, width, height );
		}

		/* We did this when we cached it. */
		ASSERT( img->w == power_of_two(img->w) );
		ASSERT( img->h == power_of_two(img->h) );

		m_iTextureWidth = m_iImageWidth = img->w;
		m_iTextureHeight = m_iImageHeight = img->h;

		/* Find a supported texture format.  If it happens to match the stored
		 * file, we won't have to do any conversion here, and that'll happen often
		 * with paletted images. */
		PixelFormat pf = img->format->BitsPerPixel == 8? PixelFormat_PAL: PixelFormat_RGB5A1;
		if( !DISPLAY->SupportsTextureFormat(pf) )
			pf = PixelFormat_RGBA4;
		ASSERT( DISPLAY->SupportsTextureFormat(pf) );

		ASSERT(img);
		m_uTexHandle = DISPLAY->CreateTexture( pf, img, false );

		CreateFrameRects();
	}

	void Destroy()
	{
		if( m_uTexHandle )
			DISPLAY->DeleteTexture( m_uTexHandle );
		m_uTexHandle = 0;
	}

	void Reload()
	{
		Destroy();
		Create();
	}

	void Invalidate()
	{
		m_uTexHandle = 0; /* don't Destroy() */
	}
};

/* If a banner is cached, get its ID for use. */
RageTextureID BannerCache::LoadCachedBanner( RString sBannerPath )
{
	RageTextureID ID( GetBannerCachePath(sBannerPath) );

	if( sBannerPath == "" )
		return ID;

	LOG->Trace( "BannerCache::LoadCachedBanner(%s): %s", sBannerPath.c_str(), ID.filename.c_str() );

	/* Hack: make sure Banner::Load doesn't change our return value and end up
	 * reloading. */
	ID = Sprite::SongBannerTexture(ID);

	/* It's not in a texture.  Do we have it loaded? */
	if( g_BannerPathToImage.find(sBannerPath) == g_BannerPathToImage.end() )
	{
		/* Oops, the image is missing.  Warn and continue. */
		LOG->Warn( "Banner cache for '%s' wasn't loaded", sBannerPath.c_str() );
		return ID;
	}

	/* This is a reference to a pointer.  BannerTexture's ctor may change it
	 * when converting; this way, the conversion will end up in the map so we
	 * only have to convert once. */
	RageSurface *&img = g_BannerPathToImage[sBannerPath];
	ASSERT( img );

	int iSourceWidth = 0, iSourceHeight = 0;
	bool WasRotatedBanner = false;
	BannerData.GetValue( sBannerPath, "Width", iSourceWidth );
	BannerData.GetValue( sBannerPath, "Height", iSourceHeight );
	BannerData.GetValue( sBannerPath, "Rotated", WasRotatedBanner );
	if( iSourceWidth == 0 || iSourceHeight == 0 )
	{
		LOG->UserLog( "Cache file", sBannerPath, "couldn't be loaded." );
		return ID;
	}

	if( WasRotatedBanner )
	{
		/* We need to tell Sprite that this was originally a rotated
		 * sprite. */
		ID.filename += "(was rotated)";
	}

	/* Is the banner already in a texture? */
	if( TEXTUREMAN->IsTextureRegistered(ID) )
		return ID; /* It's all set. */

	LOG->Trace( "Loading banner texture %s; src %ix%i; image %ix%i",
		    ID.filename.c_str(), iSourceWidth, iSourceHeight, img->w, img->h );
	RageTexture *pTexture = new BannerTexture( ID, img, iSourceWidth, iSourceHeight );

	ID.Policy = RageTextureID::TEX_VOLATILE;
	TEXTUREMAN->RegisterTexture( ID, pTexture );
	TEXTUREMAN->UnloadTexture( pTexture );

	return ID;
}

static inline int closest( int num, int n1, int n2 )
{
	if( abs(num - n1) > abs(num - n2) )
		return n2;
	return n1;
}

/* Create or update the banner cache file as necessary.  If in preload mode,
 * load the cache file, too.  (This is done at startup.) */
void BannerCache::CacheBanner( RString sBannerPath )
{
	if( PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_PRELOAD &&
	    PREFSMAN->m_BannerCache != BNCACHE_LOW_RES_LOAD_ON_DEMAND )
		return;

	CHECKPOINT_M( sBannerPath );
	if( !DoesFileExist(sBannerPath) )
		return;

	const RString CachePath = GetBannerCachePath(sBannerPath);

	/* Check the full file hash.  If it's the loaded and identical, don't recache. */
	if( DoesFileExist(CachePath) )
	{
		bool bCacheUpToDate = PREFSMAN->m_bFastLoad;
		if( !bCacheUpToDate )
		{
			unsigned CurFullHash;
			const unsigned FullHash = GetHashForFile( sBannerPath );
			if( BannerData.GetValue( sBannerPath, "FullHash", CurFullHash ) && CurFullHash == FullHash )
				bCacheUpToDate = true;
		}

		if( bCacheUpToDate )
		{
			/* It's identical.  Just load it, if in preload. */
			if( PREFSMAN->m_BannerCache == BNCACHE_LOW_RES_PRELOAD )
				LoadBanner( sBannerPath );

			return;
		}
	}

	/* The cache file doesn't exist, or is out of date.  Cache it.  This
	 * will also load the cache into memory if in PRELOAD. */
	CacheBannerInternal( sBannerPath );
}

void BannerCache::CacheBannerInternal( RString sBannerPath )
{
	RString error;
	RageSurface *img = RageSurfaceUtils::LoadFile( sBannerPath, error );
	if( img == NULL )
	{
		LOG->UserLog( "Cache file", sBannerPath, "couldn't be loaded: %s", error.c_str() );
		return;
	}

	bool WasRotatedBanner = false;

	if( Sprite::IsDiagonalBanner(img->w , img->h) )
	{
		/* Ack.  It's a diagonal banner.  Problem: if we resize a diagonal banner, we
		 * get ugly checker patterns.  We need to un-rotate it.
		 *
		 * If we spin the banner by hand, we need to do a linear filter, or the
		 * fade to the full resolution banner is misaligned, which looks strange.
		 *
		 * To do a linear filter, we need to lose the palette.  Oh well.
		 *
		 * This also makes the banner take less memory, though that could also be
		 * done by RLEing the surface.
		 */
		RageSurfaceUtils::ApplyHotPinkColorKey( img );

		RageSurfaceUtils::ConvertSurface(img, img->w, img->h, 32, 0x000000FF, 0x0000FF00, 0x00FF0000, 0xFF000000);
		
		RageSurface *dst = CreateSurface(
			256, 64, img->format->BitsPerPixel, 
			img->format->Rmask, img->format->Gmask, img->format->Bmask, img->format->Amask );

		if( img->format->BitsPerPixel == 8 ) 
		{
			ASSERT( img->format->palette );
			dst->fmt.palette = img->fmt.palette;
		}

		const float fCustomImageCoords[8] = {
			0.02f,	0.78f,	// top left
			0.22f,	0.98f,	// bottom left
			0.98f,	0.22f,	// bottom right
			0.78f,	0.02f,	// top right
		};

		RageSurfaceUtils::BlitTransform( img, dst, fCustomImageCoords );

		delete img;
		img = dst;

		WasRotatedBanner = true;
	}



	const int iSourceWidth = img->w, iSourceHeight = img->h;

	int width = img->w / 2, height = img->h / 2;
//	int width = img->w, height = img->h;

	/* Round to the nearest power of two.  This simplifies the actual texture load. */
	width = closest(width, power_of_two(width), power_of_two(width) / 2);
	height = closest(height, power_of_two(height), power_of_two(height) / 2);

	/* Don't resize the image to less than 32 pixels in either dimension or the next
	 * power of two of the source (whichever is smaller); it's already very low res. */
	width = max( width, min(32, power_of_two(iSourceWidth)) );
	height = max( height, min(32, power_of_two(iSourceHeight)) );

	RageSurfaceUtils::ApplyHotPinkColorKey( img );

	RageSurfaceUtils::Zoom( img, width, height );

	/*
	 * When paletted banner cache is enabled, cached banners are paletted.  Cached
	 * 32-bit banners take 1/16 as much memory, 16-bit banners take 1/8, and paletted
	 * banners take 1/4.
	 *
	 * When paletted banner cache is disabled, cached banners are stored in 16-bit
	 * RGBA.  Cached 32-bit banners take 1/8 as much memory, cached 16-bit banners
	 * take 1/4, and cached paletted banners take 1/2.
	 *
	 * Paletted cache is disabled by default because palettization takes time, causing
	 * the initial cache run to take longer.  Also, newer ATI hardware doesn't supported
	 * paletted textures, which would slow down runtime, because we have to depalettize
	 * on use.  They'd still have the same memory benefits, though, since we only load
	 * one cached banner into a texture at once, and the speed hit may not matter on
	 * newer ATI cards.  RGBA is safer, though.
	 */
	if( PREFSMAN->m_bPalettedBannerCache )
	{
		if( img->fmt.BytesPerPixel != 1 )
			RageSurfaceUtils::Palettize( img );
	}
	else
	{
		/* Dither to the final format.  We use A1RGB5, since that's usually supported
		 * natively by both OpenGL and D3D. */
		RageSurface *dst = CreateSurface( img->w, img->h, 16,
			0x7C00, 0x03E0, 0x001F, 0x8000 );

		/* OrderedDither is still faster than ErrorDiffusionDither, and
		 * these images are very small and only displayed briefly. */
		RageSurfaceUtils::OrderedDither( img, dst );
		delete img;
		img = dst;
	}

	const RString CachePath = GetBannerCachePath(sBannerPath);
	RageSurfaceUtils::SaveSurface( img, CachePath );

	/* If an old image is loaded, free it. */
	if( g_BannerPathToImage.find(sBannerPath) != g_BannerPathToImage.end() )
	{
		RageSurface *oldimg = g_BannerPathToImage[sBannerPath];
		delete oldimg;
		g_BannerPathToImage.erase(sBannerPath);
	}

	if( PREFSMAN->m_BannerCache == BNCACHE_LOW_RES_PRELOAD )
	{
		/* Keep it; we're just going to load it anyway. */
		g_BannerPathToImage[sBannerPath] = img;
	}
	else
		delete img;

	/* Remember the original size. */
	BannerData.SetValue( sBannerPath, "Path", CachePath );
	BannerData.SetValue( sBannerPath, "Width", iSourceWidth );
	BannerData.SetValue( sBannerPath, "Height", iSourceHeight );
	BannerData.SetValue( sBannerPath, "FullHash", GetHashForFile( sBannerPath ) );
	/* Remember this, so we can hint Sprite. */
	BannerData.SetValue( sBannerPath, "Rotated", WasRotatedBanner );
	BannerData.WriteFile( BANNER_CACHE_INDEX );
}

/*
 * (c) 2003 Glenn Maynard
 * All rights reserved.
 * 
 * Permission is hereby granted, free of charge, to any person obtaining a
 * copy of this software and associated documentation files (the
 * "Software"), to deal in the Software without restriction, including
 * without limitation the rights to use, copy, modify, merge, publish,
 * distribute, and/or sell copies of the Software, and to permit persons to
 * whom the Software is furnished to do so, provided that the above
 * copyright notice(s) and this permission notice appear in all copies of
 * the Software and that both the above copyright notice(s) and this
 * permission notice appear in supporting documentation.
 * 
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
 * OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF
 * MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT OF
 * THIRD PARTY RIGHTS. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR HOLDERS
 * INCLUDED IN THIS NOTICE BE LIABLE FOR ANY CLAIM, OR ANY SPECIAL INDIRECT
 * OR CONSEQUENTIAL DAMAGES, OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS
 * OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR
 * OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR
 * PERFORMANCE OF THIS SOFTWARE.
 */
