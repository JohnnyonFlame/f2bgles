/*
 * Fade To Black engine rewrite
 * Copyright (C) 2006-2012 Gregory Montoir (cyx@users.sourceforge.net)
 */

#ifdef USE_GLES
#include <GLES/gl.h>
#else
#include <SDL_opengl.h>
#endif
#include "scaler.h"
#include "texturecache.h"
#include <iostream>
#include <fstream>
#include <SDL.h>

static const int kDefaultTexBufSize = 320 * 200;
static const int kTextureMinMaxFilter = GL_NEAREST; // GL_NEAREST

uint16_t convert_RGBA_5551(int r, int g, int b) {
	return ((r >> 3) << 11) | ((g >> 3) << 6) | ((b >> 3) << 1) | 1;
}

uint16_t convert_BGRA_1555(int r, int g, int b) {
	return 0x8000 | ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3);
}

static const struct {
	int internal;
	int format;
	int type;
	uint16_t (*convertColor)(int, int, int);
} _formats[] = {
#ifdef __amigaos4__
	{ GL_RGB5_A1, GL_BGRA, GL_UNSIGNED_SHORT_1_5_5_5_REV, &convert_BGRA_1555 },
#endif
#ifdef USE_GLES
	{ GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, &convert_RGBA_5551 },
#else
	{ GL_RGBA, GL_RGBA, GL_UNSIGNED_SHORT_5_5_5_1, &convert_RGBA_5551 },
#endif
	{ -1, -1, -1, 0 }
};

static const struct {
	void (*proc)(uint16_t *dst, int dstPitch, const uint16_t *src, int srcPitch, int w, int h);
        int factor;
} _scalers[] = {
	{ point1x, 1 },
	{ point2x, 2 },
	{ scale2x, 2 },
	{ point3x, 3 },
	{ scale3x, 3 },
};

static const int _scaler = 0;

Atlas::Atlas(GLint maxTexSz, int fmt, Atlas *next)
{
	glGenTextures(1, &this->tex);
	glBindTexture(GL_TEXTURE_2D, this->tex);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
	glPixelStorei(GL_PACK_ALIGNMENT, 1);
	glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
	glTexImage2D(GL_TEXTURE_2D, 0, _formats[fmt].internal, maxTexSz, maxTexSz, 0, _formats[fmt].format, _formats[fmt].type, NULL);
	glBindTexture(GL_TEXTURE_2D, 0);
	
	tree = new AtlasNode(0, 0, maxTexSz, maxTexSz);
}

Atlas::~Atlas()
{
	glDeleteTextures(1, &tex);
	delete tree;
}

AtlasNode::AtlasNode(int x, int y, int w, int h)
{
	this->occupied = 0;
	this->x = x;
	this->y = y;
	this->w = w;
	this->h = h;
	
	this->children[0] = NULL;
	this->children[1] = NULL;
}

AtlasNode::~AtlasNode()
{
	if (children[0])
		delete children[0];
	if (children[1])
		delete children[1];
}

AtlasNode *AtlasNode::findFreeNode(int w, int h)
{	
	AtlasNode *node = NULL;
	if ((this->occupied == 0) && this->w >= w && this->h >= h)
		return this;
	
	if (this->children[0]         ) node = this->children[0]->findFreeNode(w, h);
	if (this->children[1] && !node) node = this->children[1]->findFreeNode(w, h);
	
	return node;
}

void AtlasNode::splitNode(int w, int h)
{
	int dw = this->w - w;
	int dh = this->h - h;
	
	if (dw > dh) //choose biggest difference as node split
	{
		this->children[0] = new AtlasNode(this->x + w, this->y, this->w - w, h);
		this->children[1] = new AtlasNode(this->x, this->y + h, this->w, this->h - h);
	}
	else
	{
		this->children[1] = new AtlasNode(this->x + w, this->y, this->w - w, this->h);
		this->children[0] = new AtlasNode(this->x, this->y + h, w, this->h - h);
	}
	
	this->w = w;
	this->h = h;
	
	this->occupied = 1;
}

TextureCache::TextureCache()
	: _fmt(0), _texturesListHead(0), _texturesListTail(0) {
	memset(_clut, 0, sizeof(_clut));
	if (_scalers[_scaler].factor != 1) {
		_texBuf = (uint16_t *)malloc(kDefaultTexBufSize * sizeof(uint16_t));
	} else {
		_texBuf = 0;
	}
	_npotTex = false;
}

TextureCache::~TextureCache() {
	free(_texBuf);
	flush();
}

static bool hasExt(const char *exts, const char *name) {
	const char *p = strstr(exts, name);
	if (p) {
		p += strlen(name);
		return *p == ' ' || *p == 0;
	}
	return false;
}

void TextureCache::init() {
	const char *exts = (const char *)glGetString(GL_EXTENSIONS);
	if (exts && hasExt(exts, "GL_ARB_texture_non_power_of_two")) {
		_npotTex = true;
	}
	
	glGetIntegerv(GL_MAX_TEXTURE_SIZE, &maxTexSz);
	maxTexSz = 4096;
	atlas = new Atlas(maxTexSz, _fmt, NULL);
}

void TextureCache::flush() {
	Texture *t = _texturesListHead;
	while (t) {
		Texture *next = t->next;
		free(t->bitmapData);
		delete t;
		t = next;
	}
	_texturesListHead = _texturesListTail = 0;
	memset(_clut, 0, sizeof(_clut));
	delete atlas;
	
	atlas = new Atlas(maxTexSz, _fmt, NULL);
}

Texture *TextureCache::getCachedTexture(const uint8_t *data, int w, int h, int16_t key) {
	Texture *prev = 0;
	for (Texture *t = _texturesListHead; t; t = t->next) {
		if (t->key == key) {
			if (prev) { // move to head
				prev->next = t->next;
				t->next = _texturesListHead;
				_texturesListHead = t;
				if (t == _texturesListTail) {
					_texturesListTail = prev;
				}
			}
			return t;
		}
		prev = t;
	}
	Texture *t = createTexture(data, w, h);
	if (t) {
		t->key = key;
	}
	return t;
}

static int roundPow2(int sz) {
	if (sz != 0 && (sz & (sz - 1)) == 0) {
		return sz;
	}
	int textureSize = 1;
	while (textureSize < sz) {
		textureSize <<= 1;
	}
	return textureSize;
}

void TextureCache::convertTexture(const uint8_t *src, int w, int h, const uint16_t *clut, uint16_t *dst, int dstPitch) {
	if (_scalers[_scaler].factor == 1) {
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				dst[x] = clut[src[x]];
			}
			dst += dstPitch;
			src += w;
		}
	} else {
		assert(w * h <= kDefaultTexBufSize);
		for (int y = 0; y < h; ++y) {
			for (int x = 0; x < w; ++x) {
				_texBuf[y * w + x] = clut[src[x]];
			}
			src += w;
		}
		_scalers[_scaler].proc(dst, dstPitch, _texBuf, w, w, h);
	}
}

Texture *TextureCache::createTexture(const uint8_t *data, int w, int h) {
	AtlasNode *node = NULL;
	Texture *t = new Texture;
	t->bitmapW = w;
	t->bitmapH = h;
	t->bitmapData = (uint8_t *)malloc(w * h);
	if (!t->bitmapData) {
		delete t;
		return 0;
	}
	
	memcpy(t->bitmapData, data, w * h);
	w *= _scalers[_scaler].factor;
	h *= _scalers[_scaler].factor;
	
	node = atlas->tree->findFreeNode(w, h);
	if (!node) {
		return 0;
	}
	node->splitNode(w, h);
	
	t->texX = node->x;
	t->texY = node->y;
	t->texW = w;
	t->texH = h;
	t->x = t->texX / (float)maxTexSz;
	t->y = t->texY / (float)maxTexSz;
	t->u = t->x + t->texW / (float)maxTexSz;
	t->v = t->y + t->texH / (float)maxTexSz;
	
	t->id = atlas->tex;
	
	glGetError();
	
	uint16_t *texData = (uint16_t *)malloc(t->texW * t->texH * sizeof(uint16_t));
	convertTexture(t->bitmapData, t->bitmapW, t->bitmapH, _clut, texData, t->texW);
	glBindTexture(GL_TEXTURE_2D, atlas->tex);
	glTexSubImage2D(GL_TEXTURE_2D, 0, t->texX, t->texY, t->texW, t->texH, _formats[_fmt].format, _formats[_fmt].type, texData);
	glBindTexture(GL_TEXTURE_2D, 0);
	
	if (!_texturesListHead) {
		_texturesListHead = _texturesListTail = t;
	} else {
		_texturesListTail->next = t;
		_texturesListTail = t;
	}
	t->next = 0;
	t->key = -1;		
	
	free(texData);
	return t;
}

void TextureCache::destroyTexture(Texture *texture) {
	free(texture->bitmapData);
	if (texture == _texturesListHead) {
		_texturesListHead = texture->next;
		if (texture == _texturesListTail) {
			_texturesListTail = _texturesListHead;
		}
	} else {
		for (Texture *t = _texturesListHead; t; t = t->next) {
			if (t->next == texture) {
				t->next = texture->next;
				if (texture == _texturesListTail) {
					_texturesListTail = t;
				}
				break;
			}
		}
	}
	delete texture;
}

void TextureCache::updateTexture(Texture *t, const uint8_t *data, int w, int h) {
	assert(t->bitmapW == w && t->bitmapH == h);
	memcpy(t->bitmapData, data, w * h);
	uint16_t *texData = (uint16_t *)malloc(t->texW * t->texH * sizeof(uint16_t));
	if (texData) {
		convertTexture(t->bitmapData, t->bitmapW, t->bitmapH, _clut, texData, t->texW);
		glBindTexture(GL_TEXTURE_2D, t->id);
		glTexSubImage2D(GL_TEXTURE_2D, 0, t->texX, t->texY, t->texW, t->texH, _formats[_fmt].format, _formats[_fmt].type, texData);
		glBindTexture(GL_TEXTURE_2D, 0);
		free(texData);
	}
}

void TextureCache::setPalette(const uint8_t *pal, bool updateTextures) {
	for (int i = 0; i < 256; ++i, pal += 3) {
		const int r = pal[0];
		const int g = pal[1];
		const int b = pal[2];
		if (r == 0 && g == 0 && b == 0) {
			_clut[i] = 0;
		} else {
			_clut[i] = _formats[_fmt].convertColor(r, g, b);
		}
	}
	
	if (updateTextures) {
		for (Texture *t = _texturesListHead; t; t = t->next) {
			uint16_t *texData = (uint16_t *)malloc(t->texW * t->texH * sizeof(uint16_t));
			if (texData) {
				convertTexture(t->bitmapData, t->bitmapW, t->bitmapH, _clut, texData, t->texW);
				glBindTexture(GL_TEXTURE_2D, t->id);
				glTexSubImage2D(GL_TEXTURE_2D, 0, t->texX, t->texY, t->texW, t->texH, _formats[_fmt].format, _formats[_fmt].type, texData);
				glBindTexture(GL_TEXTURE_2D, 0);
				free(texData);
			}
		}
	}
}
