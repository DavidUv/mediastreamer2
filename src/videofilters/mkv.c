#define bool_t ms_bool_t
#include "mediastreamer2/msfilter.h"
#include "mediastreamer2/msvideo.h"
#include "mediastreamer2/rfc3984.h"
#include "mediastreamer2/msticker.h"
#include "../audiofilters/waveheader.h"
#include "mediastreamer2/formats.h"
#undef bool_t

#define bool_t matroska_bool_t
#include <matroska/matroska.h>
#include <matroska/matroska_sem.h>
#undef bool_t

#define bool_t ambigous use ms_bool_t or matroska_bool_t

/*********************************************************************************************
 * Module interface                                                                          *
 *********************************************************************************************/
typedef void (*ModuleInitFunc)(void **obj);
typedef void (*ModuleUninitFunc)(void *obj);
typedef void (*ModuleSetFunc)(void *obj, const void *data);
typedef void (*ModulePreProFunc)(void *obj, MSQueue *input, MSQueue *output);
typedef mblk_t *(*ModuleProFunc)(void *obj, mblk_t *buffer, ms_bool_t *isKeyFrame);
typedef void (*ModulePrivateDataFunc)(const void *obj, uint8_t **data, size_t *data_size);
typedef void (*ModulePrivateDataLoadFunc)(void *obj, const uint8_t *data);
typedef ms_bool_t (*ModuleIsKeyFrameFunc)(const mblk_t *frame);

typedef struct
{
	const char *rfcName;
	const char *codecId;
	ModuleInitFunc init;
	ModuleUninitFunc uninit;
	ModuleSetFunc set;
	ModulePreProFunc preprocess;
	ModuleProFunc process;
	ModulePrivateDataFunc get_private_data;
	ModulePrivateDataLoadFunc load_private_data;
	ModuleIsKeyFrameFunc is_key_frame;
} ModuleDesc;

/*********************************************************************************************
 * h264 module                                                                               *
 *********************************************************************************************/
/* H264Private */
typedef struct
{
	uint8_t profile;
	uint8_t level;
	uint8_t NALULenghtSizeMinusOne;
	MSList *sps_list;
	MSList *pps_list;
} H264Private;

static void H264Private_init(H264Private *obj)
{
	obj->NALULenghtSizeMinusOne = 0xFF;
	obj->pps_list = NULL;
	obj->sps_list = NULL;
}

static inline ms_bool_t mblk_is_equal(mblk_t *b1, mblk_t *b2)
{
	return msgdsize(b1) == msgdsize(b2) && memcmp(b1->b_rptr, b2->b_rptr, msgdsize(b1)) == 0;
}

static void H264Private_addSPS(H264Private *obj, mblk_t *sps)
{
	MSList *element;
	for(element = obj->sps_list; element != NULL && mblk_is_equal((mblk_t *)element->data, sps); element = element->next);
	if(element != NULL || obj->sps_list == NULL)
	{
		obj->profile = sps->b_rptr[1];
		obj->level = sps->b_rptr[3];
		obj->sps_list = ms_list_append(obj->sps_list, sps);
	}
}

static void H264Private_addPPS(H264Private *obj, mblk_t *pps)
{
	MSList *element;
	for(element = obj->pps_list; element != NULL && mblk_is_equal((mblk_t *)element->data, pps); element = element->next);
	if(element != NULL || obj->pps_list == NULL)
	{
		obj->pps_list = ms_list_append(obj->pps_list, pps);
	}
}

static void H264Private_serialize(const H264Private *obj, uint8_t **data, size_t *size)
{
	*size = 7;

	uint8_t nbSPS = ms_list_size(obj->sps_list);
	uint8_t nbPPS = ms_list_size(obj->pps_list);

	*size += (nbSPS + nbPPS) * 2;

	MSList *it;
	for(it=obj->sps_list;it!=NULL;it=it->next)
	{
		mblk_t *buff = (mblk_t*)(it->data);
		*size += msgdsize(buff);
	}
	for(it=obj->pps_list;it!=NULL;it=it->next)
	{
		mblk_t *buff = (mblk_t*)(it->data);
		*size += msgdsize(buff);
	}

	uint8_t *result = (uint8_t *)ms_new0(uint8_t, *size);
	result[0] = 0x01;
	result[1] = obj->profile;
	result[3] = obj->level;
	result[4] = obj->NALULenghtSizeMinusOne;
	result[5] = nbSPS & 0x1F;

	int i=6;
	for(it=obj->sps_list; it!=NULL; it=it->next)
	{
		mblk_t *buff = (mblk_t*)(it->data);
		size_t buff_size = msgdsize(buff);
		uint16_t buff_size_be = htons(buff_size);
		memcpy(&result[i], &buff_size_be, sizeof(buff_size_be));
		i+=sizeof(buff_size_be);
		memcpy(&result[i], buff->b_rptr, buff_size);
		i += buff_size;
	}

	result[i] = nbPPS;
	i++;

	for(it=obj->pps_list; it!=NULL; it=it->next)
	{
		mblk_t *buff = (mblk_t*)(it->data);
		int buff_size = msgdsize(buff);
		uint16_t buff_size_be = htons(buff_size);
		memcpy(&result[i], &buff_size_be, sizeof(buff_size_be));
		i+=sizeof(buff_size_be);
		memcpy(&result[i], buff->b_rptr, buff_size);
		i += buff_size;
	}
	*data = result;
}

static void H264Private_load(H264Private *obj, const uint8_t *data)
{
	int i;

	obj->profile = data[1];
	obj->level = data[3];
	obj->NALULenghtSizeMinusOne = data[4];

	int nbSPS = data[5] & 0x1F;
	const uint8_t *r_ptr = data + 6;
	for(i=0;i<nbSPS;i++)
	{
		uint16_t nalu_size;
		memcpy(&nalu_size, r_ptr, sizeof(uint16_t)); r_ptr += sizeof(uint16_t);
		nalu_size = ntohs(nalu_size);
		mblk_t *nalu = allocb(nalu_size, 0);
		memcpy(nalu->b_wptr, r_ptr, nalu_size); nalu->b_wptr += nalu_size; r_ptr += nalu_size;
		obj->sps_list = ms_list_append(obj->sps_list, nalu);
	}

	int nbPPS = *r_ptr; r_ptr += 1;
	for(i=0;i<nbPPS;i++)
	{
		uint16_t nalu_size;
		memcpy(&nalu_size, r_ptr, sizeof(uint16_t)); r_ptr += sizeof(uint16_t);
		nalu_size = ntohs(nalu_size);
		mblk_t *nalu = allocb(nalu_size, 0);
		memcpy(nalu->b_wptr, r_ptr, nalu_size); nalu->b_wptr += nalu_size; r_ptr += nalu_size;
		obj->pps_list = ms_list_append(obj->pps_list, nalu);
	}
}

static void H264Private_uninit(H264Private *obj)
{
	ms_list_free_with_data(obj->sps_list,(void (*)(void *))freemsg);
	ms_list_free_with_data(obj->pps_list,(void (*)(void *))freemsg);
}

/* h264 module */
typedef struct
{
	Rfc3984Context rfc3984Context;
	H264Private codecPrivate;
} H264Module;

static void h264_module_init(void **data)
{

	H264Module *mod = ms_new(H264Module, 1);
	rfc3984_init(&mod->rfc3984Context);
	H264Private_init(&mod->codecPrivate);
	*data = mod;
}

static void h264_module_uninit(void *data)
{
	H264Module *obj = (H264Module *)data;
	rfc3984_uninit(&obj->rfc3984Context);
	H264Private_uninit(&obj->codecPrivate);
	ms_free(obj);
}

static void h264_module_preprocessing(void *data, MSQueue *input, MSQueue *output)
{
	H264Module *obj = (H264Module *)data;
	MSQueue queue;
	mblk_t *inputBuffer;

	ms_queue_init(&queue);
	while((inputBuffer = ms_queue_get(input)) != NULL)
	{
		rfc3984_unpack(&obj->rfc3984Context, inputBuffer, &queue);
		if(!ms_queue_empty(&queue))
		{
			mblk_t *frame = ms_queue_get(&queue);
			mblk_t *end = frame;
			while(!ms_queue_empty(&queue))
			{
				end = concatb(end, ms_queue_get(&queue));
			}
			ms_queue_put(output, frame);
		}
	}
}

static inline int h264_nalu_type(const mblk_t *nalu)
{
	return (nalu->b_rptr[0]) & ((1<<5)-1);
}

static ms_bool_t h264_is_key_frame(const mblk_t *frame)
{
	const mblk_t *curNalu;
	for(curNalu = frame; curNalu != NULL && h264_nalu_type(curNalu) != 5; curNalu = curNalu->b_cont);
	return curNalu != NULL;
}

static void nalus_to_frame(mblk_t *buffer, mblk_t **frame, mblk_t **sps, mblk_t **pps, ms_bool_t *isKeyFrame)
{
	mblk_t *curNalu;
	*frame = NULL;
	*sps = NULL;
	*pps = NULL;
	*isKeyFrame = FALSE;
	uint32_t timecode = mblk_get_timestamp_info(buffer);

	for(curNalu = buffer; curNalu != NULL;)
	{
		mblk_t *buff = curNalu;
		curNalu = curNalu->b_cont;
		buff->b_cont = NULL;

		int type = h264_nalu_type(buff);
		switch(h264_nalu_type(buff))
		{
		case 7:
			if(*sps == NULL)
				*sps = buff;
			else
				concatb(*sps, buff);
			break;

		case 8:
			if(*pps == NULL)
				*pps = buff;
			else
				concatb(*pps, buff);
			break;

		default:
			if(type == 5)
			{
				*isKeyFrame = TRUE;
			}
			uint32_t bufferSize = htonl(msgdsize(buff));
			mblk_t *size = allocb(4, 0);
			memcpy(size->b_wptr, &bufferSize, sizeof(bufferSize));
			size->b_wptr = size->b_wptr + sizeof(bufferSize);
			concatb(size, buff);
			buff = size;

			if(*frame == NULL)
				*frame = buff;
			else
				concatb(*frame, buff);
		}
	}

	if(*frame != NULL)
	{
		msgpullup(*frame, -1);
		mblk_set_timestamp_info(*frame, timecode);
	}
	if(*sps != NULL)
	{
		msgpullup(*sps, -1);
		mblk_set_timestamp_info(*sps, timecode);
	}
	if(*pps != NULL)
	{
		msgpullup(*pps, -1);
		mblk_set_timestamp_info(*pps, timecode);
	}
}

static mblk_t *h264_module_processing(void *data, mblk_t *nalus, ms_bool_t *isKeyFrame)
{
	H264Module *obj = (H264Module *)data;
	mblk_t *frame, *sps=NULL, *pps=NULL;
	nalus_to_frame(nalus, &frame, &sps, &pps, isKeyFrame);
	if(sps != NULL)
	{
		H264Private_addSPS(&obj->codecPrivate, sps);
	}
	if(pps != NULL)
	{
		H264Private_addPPS(&obj->codecPrivate, pps);
	}
	return frame;
}

static void h264_module_get_private_data(const void *o, uint8_t **data, size_t *data_size)
{
	const H264Module *obj = (const H264Module *)o;
	H264Private_serialize(&obj->codecPrivate, data, data_size);
}

static void h264_module_load_private_data(void *o, const uint8_t *data)
{
	H264Module *obj = (H264Module *)o;
	H264Private_load(&obj->codecPrivate, data);
}

/* h264 module description */
const ModuleDesc h264_module_desc = {
	"H264",
	"V_MPEG4/ISO/AVC",
	h264_module_init,
	h264_module_uninit,
	NULL,
	h264_module_preprocessing,
	h264_module_processing,
	h264_module_get_private_data,
	h264_module_load_private_data,
	h264_is_key_frame
};

/*********************************************************************************************
 * µLaw module                                                                               *
 *********************************************************************************************/
/* WavPrivate */
typedef struct
{
	uint16_t wFormatTag;
	uint16_t nbChannels;
	uint32_t nSamplesPerSec;
	uint32_t nAvgBytesPerSec;
	uint16_t nBlockAlign;
	uint16_t wBitsPerSample;
	uint16_t cbSize;
} WavPrivate;

static void wav_private_set(WavPrivate *data, const MSFmtDescriptor *obj)
{
	uint16_t bitsPerSample = 8;
	uint16_t nbBlockAlign = (bitsPerSample * obj->nchannels)/8;
	uint32_t bitrate = bitsPerSample * obj->nchannels * obj->rate;

	data->wFormatTag = le_uint16((uint16_t)7);
	data->nbChannels = le_uint16((uint16_t)obj->nchannels);
	data->nSamplesPerSec = le_uint32((uint32_t)obj->rate);
	data->nAvgBytesPerSec = le_uint32(bitrate);
	data->nBlockAlign = le_uint16((uint16_t)nbBlockAlign);
	data->wBitsPerSample = le_uint16(bitsPerSample);
	data->cbSize = 0;
}

static void wav_private_serialize(const WavPrivate *obj, uint8_t **data, size_t *size)
{
	*size = 22;
	*data = (uint8_t *)ms_new0(uint8_t, *size);
	memcpy(*data, obj, *size);
}

static inline void wav_private_load(WavPrivate *obj, const uint8_t *data)
{
	memcpy(obj, data, 22);
}

/* µLaw module */
typedef struct
{
	WavPrivate codecPrivate;
} MuLawModule;

static void mu_law_module_init(void **o)
{
	*o = ms_new(MuLawModule, 1);
}

static void mu_law_module_uninit(void *o)
{
	ms_free(o);
}

static void mu_law_module_set(void *o, const void *data)
{
	MuLawModule *obj = (MuLawModule *)o;
	const MSFmtDescriptor *aMeta = (const MSFmtDescriptor *)data;
	wav_private_set(&obj->codecPrivate, aMeta);
}

static void mu_law_module_get_private_data(const void *o, uint8_t **data, size_t *data_size)
{
	const MuLawModule *obj = (const MuLawModule *)o;
	wav_private_serialize(&obj->codecPrivate, data, data_size);
}

static void mu_law_module_load_private(void *o, const uint8_t *data)
{
	MuLawModule *obj = (MuLawModule *)o;
	wav_private_load(&obj->codecPrivate, data);
}

/* µLaw module description */
const ModuleDesc mu_law_module_desc = {
	"pcmu",
	"A_MS/ACM",
	mu_law_module_init,
	mu_law_module_uninit,
	mu_law_module_set,
	NULL,
	NULL,
	mu_law_module_get_private_data,
	mu_law_module_load_private,
	NULL
};

/*********************************************************************************************
 * Available modules                                                                         *
 *********************************************************************************************/
typedef enum {
	H264_MOD_ID,
	MU_LAW_MOD_ID
} ModuleId;

const ModuleDesc const *modules[] = {
	&h264_module_desc,
	&mu_law_module_desc,
	NULL
};

static int find_module_id(const char rfcName[])
{
	int i;
	for(i=0; modules[i] != NULL && strcmp(modules[i]->rfcName, rfcName) != 0; i++);
	if(modules[i] == NULL)
	{
		return -1;
	}
	else
	{
		return i;
	}
}

static int find_module_id_by_codec_id(const char codecId[])
{
	int i;
	for(i=0; modules[i] != NULL && strcmp(modules[i]->codecId, codecId) != 0; i++);
	if(modules[i] == NULL)
	{
		return -1;
	}
	else
	{
		return i;
	}
}

/*********************************************************************************************
 * Matroska                                                                                  *
 *********************************************************************************************/
#define WRITE_DEFAULT_ELEMENT TRUE

static const timecode_t MKV_TIMECODE_SCALE = 1000000;
static const int MKV_DOCTYPE_VERSION = 2;
static const int MKV_DOCTYPE_READ_VERSION = 2;

extern const nodemeta LangStr_Class[];
extern const nodemeta UrlPart_Class[];
extern const nodemeta BufStream_Class[];
extern const nodemeta MemStream_Class[];
extern const nodemeta Streams_Class[];
extern const nodemeta File_Class[];
extern const nodemeta Stdio_Class[];
extern const nodemeta Matroska_Class[];
extern const nodemeta EBMLElement_Class[];
extern const nodemeta EBMLMaster_Class[];
extern const nodemeta EBMLBinary_Class[];
extern const nodemeta EBMLString_Class[];
extern const nodemeta EBMLInteger_Class[];
extern const nodemeta EBMLCRC_Class[];
extern const nodemeta EBMLDate_Class[];
extern const nodemeta EBMLVoid_Class[];

static void loadModules(nodemodule *modules)
{
	NodeRegisterClassEx(modules, Streams_Class);
	NodeRegisterClassEx(modules, File_Class);
	NodeRegisterClassEx(modules, Matroska_Class);
	NodeRegisterClassEx(modules, EBMLElement_Class);
	NodeRegisterClassEx(modules, EBMLMaster_Class);
	NodeRegisterClassEx(modules, EBMLBinary_Class);
	NodeRegisterClassEx(modules, EBMLString_Class);
	NodeRegisterClassEx(modules, EBMLInteger_Class);
	NodeRegisterClassEx(modules, EBMLCRC_Class);
	NodeRegisterClassEx(modules, EBMLDate_Class);
	NodeRegisterClassEx(modules, EBMLVoid_Class);
}

typedef enum {
	MKV_OPEN_CREATE,
	MKV_OPEN_APPEND,
	MKV_OPEN_RO
} MatroskaOpenMode;

typedef struct
{
	parsercontext *p;
	stream *output;
	ebml_element *header, *firstCluster;
	ebml_master *segment, *cluster, *info, *tracks, *metaSeek, *cues;
	matroska_seekpoint *infoMeta, *tracksMeta, *cuesMeta;
	ebml_element *currentBlock;
	timecode_t timecodeScale;
	filepos_t segmentInfoPosition;
	int nbClusters;
} Matroska;

//static int matroska_track_get_type(const Matroska *obj, int trackNum);

static void matroska_init(Matroska *obj)
{
	obj->p = (parsercontext *)ms_new(parsercontext, 1);
	ParserContext_Init(obj->p, NULL, NULL, NULL);
	loadModules((nodemodule*)obj->p);
	MATROSKA_Init((nodecontext*)obj->p);
	obj->output = NULL;
	obj->header = NULL;
	obj->segment = NULL;
	obj->metaSeek = NULL;
	obj->infoMeta = NULL;
	obj->tracksMeta = NULL;
	obj->cuesMeta = NULL;
	obj->info = NULL;
	obj->tracks = NULL;
	obj->cues = NULL;
	obj->cluster = NULL;
	obj->currentBlock = NULL;
	obj->firstCluster = NULL;
	obj->nbClusters = 0;
}

static int ebml_reading_profile(const ebml_master *head)
{
	size_t length = EBML_ElementDataSize((ebml_element *)head, FALSE);
	char *docType = ms_new(char, length);
	EBML_StringGet((ebml_string *)EBML_MasterFindChild(head, &EBML_ContextDocType), docType, length);
	int docTypeReadVersion = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(head, &EBML_ContextDocTypeReadVersion));
	int profile;

	if(strcmp(docType, "matroska")==0)
	{
		switch (docTypeReadVersion)
		{
		case 1:
			profile = PROFILE_MATROSKA_V1;
			break;
		case 2:
			profile = PROFILE_MATROSKA_V2;
			break;
		case 3:
			profile = PROFILE_MATROSKA_V3;
			break;
		case 4:
			profile = PROFILE_MATROSKA_V4;
			break;
		default:
			profile = -1;
		}
	}
	else if(strcmp(docType, "webm"))
	{
		profile = PROFILE_WEBM;
	}
	else
	{
		profile = -1;
	}

	ms_free(docType);
	return profile;
}

static ms_bool_t matroska_create_file(Matroska *obj, const char path[])
{
	obj->header = EBML_ElementCreate(obj->p, &EBML_ContextHead, TRUE, NULL);
	obj->segment = (ebml_master *)EBML_ElementCreate(obj->p, &MATROSKA_ContextSegment, TRUE, NULL);
	obj->metaSeek = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextSeekHead, FALSE);
	obj->infoMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->tracksMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->cuesMeta = (matroska_seekpoint *)EBML_MasterAddElt(obj->metaSeek, &MATROSKA_ContextSeek, TRUE);
	obj->info = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextInfo, TRUE);
	obj->tracks = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextTracks, FALSE);
	obj->cues = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextCues, FALSE);
	obj->timecodeScale = MKV_TIMECODE_SCALE;

	MATROSKA_LinkMetaSeekElement(obj->infoMeta, (ebml_element *)obj->info);
	MATROSKA_LinkMetaSeekElement(obj->tracksMeta, (ebml_element *)obj->tracks);
	MATROSKA_LinkMetaSeekElement(obj->cuesMeta, (ebml_element *)obj->cues);

	return TRUE;
}

static ms_bool_t matroska_load_file(Matroska *obj)
{
	int upperLevels = 0;
	ebml_parser_context readContext;
	readContext.Context = &MATROSKA_ContextStream;
	readContext.EndPosition = INVALID_FILEPOS_T;
	readContext.Profile = 0;
	readContext.UpContext = NULL;

	obj->header = EBML_FindNextElement(obj->output, &readContext, &upperLevels, FALSE);
	EBML_ElementReadData(obj->header, obj->output, &readContext, FALSE, SCOPE_ALL_DATA, 0);
	readContext.Profile = ebml_reading_profile((ebml_master *)obj->header);

	obj->segment = (ebml_master *)EBML_FindNextElement(obj->output, &readContext, &upperLevels, FALSE);
	readContext.EndPosition = EBML_ElementPositionEnd((ebml_element *)obj->segment);

	ebml_parser_context readSegmentContext;
	readSegmentContext.Context = EBML_ElementContext((ebml_element *)obj->segment);
	readSegmentContext.EndPosition = EBML_ElementPositionEnd((ebml_element *)obj->segment);
	readSegmentContext.UpContext = &readContext;
	readSegmentContext.Profile = ebml_reading_profile((ebml_master *)obj->header);

	ebml_element *elt;
	for(elt = EBML_FindNextElement(obj->output, &readSegmentContext, &upperLevels, FALSE); elt != NULL; elt = EBML_FindNextElement(obj->output, &readSegmentContext, &upperLevels, FALSE))
	{
		if(EBML_ElementIsType(elt, &MATROSKA_ContextSeekHead))
		{
			EBML_ElementReadData(elt, obj->output, &readSegmentContext, FALSE, SCOPE_ALL_DATA, 0);
			EBML_MasterAppend(obj->segment, elt);
			obj->metaSeek = (ebml_master*)elt;
			matroska_seekpoint *seekPoint;
			for(seekPoint = (matroska_seekpoint *)EBML_MasterChildren(obj->metaSeek); seekPoint != NULL; seekPoint = (matroska_seekpoint *)EBML_MasterNext(seekPoint))
			{
				if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextInfo))
				{
					obj->infoMeta = seekPoint;
				}
				else if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextTracks))
				{
					obj->tracksMeta = seekPoint;
				}
				else if(MATROSKA_MetaSeekIsClass(seekPoint, &MATROSKA_ContextCues))
				{
					obj->cuesMeta = seekPoint;
				}
			}
		}
		else if(EBML_ElementIsType(elt, &MATROSKA_ContextInfo))
		{
			EBML_ElementReadData(elt, obj->output, &readSegmentContext, FALSE, SCOPE_ALL_DATA, 0);
			EBML_MasterAppend(obj->segment, elt);
			obj->info = (ebml_master*)elt;
			obj->timecodeScale = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(obj->info, &MATROSKA_ContextTimecodeScale));
			MATROSKA_LinkMetaSeekElement(obj->infoMeta, (ebml_element *)obj->info);
		}
		else if(EBML_ElementIsType(elt, &MATROSKA_ContextTracks))
		{
			EBML_ElementReadData(elt, obj->output, &readSegmentContext, FALSE, SCOPE_ALL_DATA, 0);
			EBML_MasterAppend(obj->segment, elt);
			obj->tracks = (ebml_master*)elt;
			MATROSKA_LinkMetaSeekElement(obj->tracksMeta, (ebml_element *)obj->tracks);
		}
		else if(EBML_ElementIsType(elt, &MATROSKA_ContextCues))
		{
			EBML_ElementReadData(elt, obj->output, &readSegmentContext, FALSE, SCOPE_ALL_DATA, 0);
			EBML_MasterAppend(obj->segment, elt);
			obj->cues = (ebml_master*)elt;
			MATROSKA_LinkMetaSeekElement(obj->cuesMeta, (ebml_element *)obj->cues);
		}
		else if(EBML_ElementIsType(elt, &MATROSKA_ContextCluster))
		{
			EBML_ElementReadData(elt, obj->output, &readSegmentContext, FALSE, SCOPE_PARTIAL_DATA, 0);
			EBML_MasterAppend(obj->segment, elt);
			obj->cluster = (ebml_master *)elt;
			if(obj->nbClusters == 0)
			{
				obj->firstCluster = (ebml_element*) obj->cluster;
			}
			MATROSKA_LinkClusterBlocks((matroska_cluster *)obj->cluster, obj->segment, obj->tracks, FALSE);
			obj->nbClusters++;
		}
	}
	return TRUE;
}

static int matroska_open_file(Matroska *obj, const char path[], MatroskaOpenMode mode)
{
	int err = 0;

	switch(mode)
	{
	case MKV_OPEN_CREATE:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_WRONLY | SFLAG_CREATE)) == NULL)
		{
			err = -2;
			break;
		}
		if(!matroska_create_file(obj, path))
		{
			err = -3;
		}
		break;

	case MKV_OPEN_APPEND:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_REOPEN)) == NULL)
		{
			err = -2;
			break;
		}
		if(!matroska_load_file(obj))
		{
			err = -3;
			break;
		}
		if(obj->cues == NULL)
		{
			obj->cues = (ebml_master *)EBML_ElementCreate(obj->p, &MATROSKA_ContextCues, FALSE, NULL);
		}
		if(obj->cluster == NULL)
		{
			Stream_Seek(obj->output, 0, SEEK_END);
		}
		else
		{
			Stream_Seek(obj->output, EBML_ElementPositionEnd((ebml_element *)obj->cluster), SEEK_SET);
		}
		break;

	case MKV_OPEN_RO:
		if((obj->output = StreamOpen(obj->p, path, SFLAG_RDONLY)) == NULL)
		{
			err = -2;
			break;
		}
		if(!matroska_load_file(obj))
		{
			err = -3;
			break;
		}
		break;

	default:
		err = -1;
		break;
	}
	return err;
}

static void matroska_close_file(Matroska *obj)
{
	StreamClose(obj->output);
	NodeDelete((node *)obj->header);
	NodeDelete((node *)obj->segment);

	MATROSKA_Done((nodecontext*)obj->p);
	ParserContext_Done(obj->p);
	ms_free(obj->p);
}

static void matroska_set_doctype_version(Matroska *obj, int doctypeVersion, int doctypeReadVersion)
{
	EBML_IntegerSetValue((ebml_integer*)EBML_MasterFindChild(obj->header, &EBML_ContextDocTypeVersion), doctypeVersion);
	EBML_IntegerSetValue((ebml_integer*)EBML_MasterFindChild(obj->header, &EBML_ContextDocTypeReadVersion), doctypeReadVersion);
}

static inline void matroska_write_ebml_header(Matroska *obj)
{
	EBML_ElementRender(obj->header, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
}

static void matroska_set_segment_info(Matroska *obj, const char writingApp[], const char muxingApp[], double duration)
{
	EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(obj->info, &MATROSKA_ContextTimecodeScale), obj->timecodeScale);
	EBML_StringSetValue((ebml_string*)EBML_MasterGetChild(obj->info, &MATROSKA_ContextMuxingApp), muxingApp);
	EBML_StringSetValue((ebml_string*)EBML_MasterGetChild(obj->info, &MATROSKA_ContextWritingApp), writingApp);
	EBML_FloatSetValue((ebml_float *)EBML_MasterGetChild(obj->info, &MATROSKA_ContextDuration), duration);
}

static inline timecode_t matroska_get_duration(const Matroska *obj)
{
	return (timecode_t)EBML_FloatValue((ebml_float *)EBML_MasterFindChild(obj->info, &MATROSKA_ContextDuration));
}

static void updateElementHeader(ebml_element *element, stream *file)
{
	filepos_t initial_pos = Stream_Seek(file, 0, SEEK_CUR);
	Stream_Seek(file, EBML_ElementPosition(element), SEEK_SET);
	EBML_ElementUpdateSize(element, WRITE_DEFAULT_ELEMENT, FALSE);
	EBML_ElementRenderHead(element, file, FALSE, NULL);
	Stream_Seek(file, initial_pos, SEEK_SET);
}

static inline void matroska_start_segment(Matroska *obj)
{
	EBML_ElementSetSizeLength((ebml_element *)obj->segment, 8);
	EBML_ElementRenderHead((ebml_element *)obj->segment, obj->output, FALSE, NULL);
}

static int matroska_write_zeros(Matroska *obj, size_t nbZeros)
{
	uint8_t *data = (uint8_t *)ms_new0(uint8_t, nbZeros);
	if(data == NULL)
	{
		return 0;
	}

	size_t written;
	Stream_Write(obj->output, data, nbZeros, &written);
	ms_free(data);
	return written;
}

static inline void matroska_mark_segment_info_position(Matroska *obj)
{
	obj->segmentInfoPosition = Stream_Seek(obj->output, 0, SEEK_CUR);
}

static inline void matroska_go_to_segment_info_mark(Matroska *obj)
{
	Stream_Seek(obj->output, obj->segmentInfoPosition, SEEK_SET);
}

static inline void matroska_go_to_file_end(Matroska *obj)
{
	Stream_Seek(obj->output, 0, SEEK_END);
}

static inline void matroska_go_to_segment_begin(Matroska *obj)
{
	Stream_Seek(obj->output, EBML_ElementPositionData((ebml_element *)obj->segment), SEEK_SET);
}

static inline void matroska_go_to_last_cluster_end(Matroska *obj)
{
	Stream_Seek(obj->output, EBML_ElementPositionEnd((ebml_element *)obj->cluster), SEEK_SET);
}

static inline void matroska_go_to_segment_info_begin(Matroska *obj)
{
	Stream_Seek(obj->output, EBML_ElementPosition((ebml_element *)obj->info), SEEK_SET);
}

//static int matroska_block_go_first(Matroska *obj)
//{
//	if(obj->firstCluster == NULL)
//	{
//		return -1;
//	}
//	obj->currentBlock = EBML_MasterFindChild(obj->firstCluster, &MATROSKA_ContextSimpleBlock);
//	if(obj->currentBlock == NULL)
//	{
//		return -2;
//	}
//	else
//	{
//		return 0;
//	}
//}

//static int matroska_block_go_next(Matroska *obj, ms_bool_t *eof)
//{
//	*eof = FALSE;
//	if(obj->currentBlock == NULL)
//	{
//		return -1;
//	}
//	ebml_element *currentCluster = EBML_ElementParent(obj->currentBlock);
//	obj->currentBlock = EBML_MasterFindNextElt((ebml_master *)currentCluster, obj->currentBlock, FALSE, FALSE);
//	if(obj->currentBlock != NULL)
//	{
//		return 0;
//	}
//	else
//	{
//		currentCluster = EBML_MasterFindNextElt(obj->segment, (ebml_element *)currentCluster, FALSE, FALSE);
//		if(currentCluster == NULL)
//		{
//			*eof = TRUE;
//			return 0;
//		}
//		else
//		{
//			obj->currentBlock = EBML_MasterFindChild(currentCluster, &MATROSKA_ContextSimpleBlock);
//			if(obj->currentBlock == NULL)
//			{
//				return -2;
//			}
//			else
//			{
//				return 0;
//			}
//		}
//	}
//}

//static mblk_t *matroska_block_read_frame(const Matroska *obj, int *trackType)
//{
//	matroska_frame frame;
//	matroska_block * block = EBML_ElementCopy(obj->currentBlock, NULL);
//	MATROSKA_BlockReadData(block, obj->output);
//	MATROSKA_BlockGetFrame(block, 0, &frame, TRUE);
//	mblk_t *frameBuffer = allocb(frame.Size, 0);
//	memcpy(frameBuffer->b_wptr, frame.Data, frame.Size);
//	frameBuffer->b_wptr += frame.Size;
//	mblk_set_timestamp_info(frameBuffer, frame.Timecode);
//	*trackType = matroska_track_get_type(obj, MATROSKA_BlockTrackNum(block));
//	MATROSKA_BlockReleaseData(block, TRUE);
//	NodeDelete((node *)block);
//	return frameBuffer;
//}

static int ebml_element_cmp_position(const void *a, const void *b)
{
	return EBML_ElementPosition((ebml_element *)a) - EBML_ElementPosition((ebml_element *)b);
}

static void ebml_master_sort(ebml_master *master_elt)
{
	MSList *elts = NULL;
	ebml_element *elt;
	for(elt = EBML_MasterChildren(master_elt); elt != NULL; elt = EBML_MasterNext(elt))
	{
		elts = ms_list_insert_sorted(elts, elt, (MSCompareFunc)ebml_element_cmp_position);
	}
	EBML_MasterClear(master_elt);
	MSList *it;
	for(it = elts; it != NULL; it = ms_list_next(it))
	{
		EBML_MasterAppend(master_elt, (ebml_element *)it->data);
	}
	ms_list_free(elts);
}

static int ebml_master_fill_blanks(stream *output, ebml_master *master)
{
	MSList *voids = NULL;
	ebml_element *elt1, *elt2;
	for(elt1 = EBML_MasterChildren(master), elt2 = EBML_MasterNext(elt1); elt2 != NULL; elt1 = EBML_MasterNext(elt1), elt2 = EBML_MasterNext(elt2))
	{
		filepos_t elt1_end_pos = EBML_ElementPositionEnd(elt1);
		filepos_t elt2_pos = EBML_ElementPosition(elt2);
		int interval = elt2_pos - elt1_end_pos;
		if(interval < 0)
		{
			return -1; // Elements are neither contigus or distinct.
		}
		else if(interval == 0)
		{
			// Nothing to do. Elements are contigus.
		}
		else if(interval > 0 && interval < 2)
		{
			return -2; // Not enough space to write a void element.
		}
		else
		{
			ebml_element *voidElt = EBML_ElementCreate(master, &EBML_ContextEbmlVoid, TRUE, NULL);
			EBML_VoidSetFullSize(voidElt, interval);
			Stream_Seek(output, elt1_end_pos, SEEK_SET);
			EBML_ElementRender(voidElt, output, FALSE, FALSE, FALSE, NULL);
			voids = ms_list_append(voids, voidElt);
		}
	}

	MSList *it;
	for(it = voids; it != NULL; it = ms_list_next(it))
	{
		EBML_MasterAppend(master, (ebml_element *)it->data);
	}
	ms_list_free(voids);
	return 0;
}

static void ebml_master_delete_empty_elements(ebml_master *master)
{
	ebml_element *child;
	for(child = EBML_MasterChildren(master); child != NULL; child = EBML_MasterNext(child))
	{
		if(EBML_ElementDataSize(child, WRITE_DEFAULT_ELEMENT) <= 0)
		{
			EBML_MasterRemove(master, child);
			NodeDelete((node *)child);
		}
	}
}

static int matroska_close_segment(Matroska *obj)
{
	filepos_t initialPos = Stream_Seek(obj->output, 0, SEEK_CUR);
	EBML_ElementUpdateSize(obj->segment, WRITE_DEFAULT_ELEMENT, FALSE);
	ebml_master_delete_empty_elements(obj->segment);
	ebml_master_sort(obj->segment);
	if(ebml_master_fill_blanks(obj->output, obj->segment) < 0)
	{
		return -1;
	}
	updateElementHeader((ebml_element *)obj->segment, obj->output);
	Stream_Seek(obj->output, initialPos, SEEK_SET);
	return 0;
}

static ebml_master *matroska_find_track_entry(const Matroska *obj, int trackNum)
{
	ebml_element *trackEntry;
	for(trackEntry = EBML_MasterChildren(obj->tracks);
		trackEntry != NULL && EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild((ebml_master *)trackEntry, &MATROSKA_ContextTrackNumber)) != trackNum;
		trackEntry = EBML_MasterNext(trackEntry));
	return (ebml_master *)trackEntry;
}

static ms_bool_t matroska_get_codec_private(const Matroska *obj, int trackNum, const uint8_t **data, size_t *length)
{
	ebml_master *trackEntry = matroska_find_track_entry(obj, trackNum);
	if(trackEntry == NULL)
		return FALSE;
	ebml_binary *codecPrivate = (ebml_binary *)EBML_MasterFindChild(trackEntry, &MATROSKA_ContextCodecPrivate);
	if(codecPrivate == NULL)
		return FALSE;

	*length = EBML_ElementDataSize((ebml_element *)codecPrivate, FALSE);
	*data =  EBML_BinaryGetData(codecPrivate);
	if(*data == NULL)
		return FALSE;
	else
		return TRUE;
}

static void matroska_start_cluster(Matroska *obj, timecode_t clusterTimecode)
{
	obj->cluster = (ebml_master *)EBML_MasterAddElt(obj->segment, &MATROSKA_ContextCluster, TRUE);
	if(obj->nbClusters == 0)
	{
		obj->firstCluster = (ebml_element *)obj->cluster;
	}
	EBML_ElementSetSizeLength((ebml_element *)obj->cluster, 8);
	EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode), clusterTimecode);
	EBML_ElementRender((ebml_element *)obj->cluster, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	obj->nbClusters++;
}

static void matroska_close_cluster(Matroska *obj)
{
	ebml_element *block;
	if(obj->cluster == NULL)
	{
		return;
	}
	else
	{
		block = EBML_MasterFindChild(obj->cluster, &MATROSKA_ContextSimpleBlock);
	}
	if(block == NULL)
	{
		ebml_element *voidElt = EBML_ElementCreate(obj->p, &EBML_ContextEbmlVoid, FALSE, NULL);
		EBML_MasterAppend(obj->segment, voidElt);
		EBML_VoidSetFullSize(voidElt, EBML_ElementFullSize((ebml_element *)obj->cluster, WRITE_DEFAULT_ELEMENT));
		Stream_Seek((ebml_master*)obj->output, EBML_ElementPosition((ebml_element *)obj->cluster), SEEK_SET);
		EBML_ElementRender(voidElt, obj->output, FALSE, FALSE, FALSE, NULL);
		EBML_MasterRemove(obj->segment, (ebml_element *)obj->cluster);
		NodeDelete((node *)obj->cluster);
		obj->cluster = NULL;
	}
	else
	{
		updateElementHeader((ebml_element *)obj->cluster, obj->output);
	}
}

static inline int matroska_clusters_count(const Matroska *obj)
{
	return obj->nbClusters;
}

static inline timecode_t matroska_current_cluster_timecode(const Matroska *obj)
{
	return EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(obj->cluster, &MATROSKA_ContextTimecode));
}

static ebml_element *matroska_find_track(const Matroska *obj, int trackNum)
{
	ebml_element *elt;
	for(elt = EBML_MasterChildren(obj->tracks);
		elt != NULL && EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(elt, &MATROSKA_ContextTrackNumber)) != trackNum;
		elt = EBML_MasterNext(elt));
	return elt;
}

static int matroska_add_track(Matroska *obj, int trackNum, int trackType, const char codecID[])
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track != NULL)
	{
		return -1;
	}
	else
	{
		track = EBML_MasterAddElt(obj->tracks, &MATROSKA_ContextTrackEntry, FALSE);
		if(track == NULL)
		{
			return -2;
		}
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextTrackNumber), trackNum);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextTrackUID), trackNum);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextTrackType), trackType);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextFlagEnabled), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextFlagDefault), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextFlagForced), 0);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextFlagLacing), 0);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextMinCache), 1);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextMaxBlockAdditionID), 0);
		EBML_StringSetValue((ebml_string *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextCodecID), codecID);
		EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextCodecDecodeAll), 0);
		return 0;
	}
}

static int matroska_del_track(Matroska *obj, int trackNum)
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		if(EBML_MasterRemove(obj->tracks, track) != ERR_NONE)
		{
			return -2;
		}
		else
		{
			NodeDelete((node *)track);
			return 0;
		}
	}
}

static int matroska_get_default_track_num(Matroska *obj, int trackType)
{
	ebml_element *track;
	for(track = EBML_MasterChildren(obj->tracks); track != NULL; track = EBML_MasterNext(track))
	{
		ebml_integer *trackTypeElt = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackType);
		ebml_integer *flagDefault = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextFlagDefault);
		if(trackTypeElt != NULL && flagDefault != NULL && EBML_IntegerValue(trackTypeElt) == trackType && EBML_IntegerValue(flagDefault) == 1)
		{
			break;
		}
	}
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		ebml_integer *trackNum = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackNumber);
		if(trackNum == NULL)
		{
			return -2;
		}
		else
		{
			return EBML_IntegerValue(trackNum);
		}
	}
}

static int matroska_track_set_codec_private(Matroska *obj, int trackNum, const uint8_t *data, size_t dataSize)
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		ebml_binary *codecPrivate = EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextCodecPrivate);
		if(EBML_BinarySetData(codecPrivate, data, dataSize) != ERR_NONE)
		{
			return -2;
		}
		else
		{
			return 0;
		}
	}
}

static int matroska_track_set_video_info(Matroska *obj, int trackNum, const MSFmtDescriptor *vMeta)
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		int trackType = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackType));
		if(trackType != TRACK_TYPE_VIDEO)
		{
			return -2;
		}
		else
		{
			ebml_element *videoInfo = EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextVideo);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)videoInfo, &MATROSKA_ContextFlagInterlaced), 0);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)videoInfo, &MATROSKA_ContextPixelWidth), vMeta->vsize.width);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)videoInfo, &MATROSKA_ContextPixelHeight), vMeta->vsize.height);
			return 0;
		}
	}
}

static int matroska_track_set_audio_info(Matroska *obj, int trackNum, const MSFmtDescriptor *aMeta)
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		int trackType = EBML_IntegerValue((ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackType));
		if(trackType != TRACK_TYPE_AUDIO)
		{
			return -2;
		}
		else
		{
			ebml_element *audioInfo = EBML_MasterGetChild((ebml_master*)track, &MATROSKA_ContextAudio);
			EBML_FloatSetValue((ebml_float *)EBML_MasterGetChild((ebml_master*)audioInfo, &MATROSKA_ContextSamplingFrequency), aMeta->rate);
			EBML_IntegerSetValue((ebml_integer *)EBML_MasterGetChild((ebml_master*)audioInfo, &MATROSKA_ContextChannels), aMeta->nchannels);
			return 0;
		}
	}
}

static int matroska_track_get_codec_id(const Matroska *obj, int trackNum, char *codecId, size_t size)
{
	ebml_element *track = matroska_find_track(obj, trackNum);
	if(track == NULL)
	{
		return -1;
	}
	else
	{
		ebml_string *codecIdElt = (ebml_string *)EBML_MasterFindChild(track, &MATROSKA_ContextCodecID);
		if(codecIdElt == NULL)
		{
			return -2;
		}
		else
		{
			EBML_StringGet(codecIdElt, codecId, size);
			return 0;
		}
	}
}

//static int matroska_track_get_type(const Matroska *obj, int trackNum)
//{
//	ebml_element *track = matroska_find_track(obj, trackNum);
//	if(track == NULL)
//	{
//		return -1;
//	}
//	else
//	{
//		ebml_integer *trackType = (ebml_integer *)EBML_MasterFindChild(track, &MATROSKA_ContextTrackType);
//		if(trackType == NULL)
//		{
//			return -2;
//		}
//		else
//		{
//			return EBML_IntegerValue(trackType);
//		}
//	}
//}

static int matroska_add_cue(Matroska *obj, matroska_block *block)
{
	matroska_cuepoint *cue = (matroska_cuepoint *)EBML_MasterAddElt(obj->cues, &MATROSKA_ContextCuePoint, TRUE);
	if(cue == NULL)
	{
		return -1;
	}
	else
	{
		MATROSKA_LinkCuePointBlock(cue, block);
		MATROSKA_LinkCueSegmentInfo(cue, obj->info);
		MATROSKA_CuePointUpdate(cue, (ebml_element *)obj->segment);
		return 0;
	}
}

static inline void matroska_write_segment_info(Matroska *obj)
{
	EBML_ElementRender((ebml_element *)obj->info, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_MetaSeekUpdate(obj->infoMeta);
}

static inline void matroska_write_tracks(Matroska *obj)
{
	EBML_ElementRender((ebml_element *)obj->tracks, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_MetaSeekUpdate(obj->tracksMeta);
}

static inline void matroska_write_cues(Matroska *obj)
{
	EBML_ElementRender((ebml_element *)obj->cues, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_MetaSeekUpdate(obj->cuesMeta);
}

static inline void matroska_write_metaSeek(Matroska *obj)
{
	EBML_ElementRender((ebml_element *)obj->metaSeek, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
}

static inline timecode_t matroska_get_current_cluster_timestamp(const Matroska *obj)
{
	return (timecode_t)EBML_IntegerValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode));
}

static matroska_block *matroska_write_block(Matroska *obj, const matroska_frame *m_frame, int trackNum, ms_bool_t isKeyFrame)
{
	matroska_block *block = (matroska_block *)EBML_MasterAddElt(obj->cluster, &MATROSKA_ContextSimpleBlock, FALSE);
	block->TrackNumber = trackNum;
	MATROSKA_LinkBlockWithReadTracks(block, obj->tracks, TRUE);
	MATROSKA_LinkBlockWriteSegmentInfo(block, obj->info);
	MATROSKA_BlockSetKeyframe(block, isKeyFrame);
	MATROSKA_BlockSetDiscardable(block, FALSE);
	timecode_t clusterTimecode = EBML_IntegerValue((ebml_integer *)EBML_MasterGetChild(obj->cluster, &MATROSKA_ContextTimecode));
	MATROSKA_BlockAppendFrame(block, m_frame, clusterTimecode * obj->timecodeScale);
	EBML_ElementRender((ebml_element *)block, obj->output, WRITE_DEFAULT_ELEMENT, FALSE, FALSE, NULL);
	MATROSKA_BlockReleaseData(block, TRUE);
	return block;
}

/*********************************************************************************************
 * Muxer                                                                                     *
 *********************************************************************************************/
typedef enum {
	VIDEO_BUFFER,
	AUDIO_BUFFER
} MuxerBufferType;

typedef struct
{
	MSQueue videoQueue;
	MSQueue audioQueue;
} Muxer;

static void muxer_init(Muxer *obj)
{
	ms_queue_init(&obj->videoQueue);
	ms_queue_init(&obj->audioQueue);
}

static void muxer_empty_internal_queues(Muxer *obj)
{
	ms_queue_flush(&obj->videoQueue);
	ms_queue_flush(&obj->audioQueue);
}

static void muxer_uninit(Muxer *obj)
{
	ms_queue_flush(&obj->videoQueue);
	ms_queue_flush(&obj->audioQueue);
}

static inline void muxer_put_video_buffer(Muxer *obj, mblk_t *buffer)
{
	ms_queue_put(&obj->videoQueue, buffer);
}

static inline void muxer_put_audio_buffer(Muxer *obj, mblk_t *buffer)
{
	ms_queue_put(&obj->audioQueue, buffer);
}

static mblk_t *muxer_get_buffer(Muxer *obj, MuxerBufferType *bufferType)
{
	if(ms_queue_empty(&obj->videoQueue) && ms_queue_empty(&obj->audioQueue))
	{
		return NULL;
	}
	else if(!ms_queue_empty(&obj->videoQueue) && ms_queue_empty(&obj->audioQueue))
	{
		*bufferType = VIDEO_BUFFER;
		return ms_queue_get(&obj->videoQueue);
	}
	else if(ms_queue_empty(&obj->videoQueue) && !ms_queue_empty(&obj->audioQueue))
	{
		*bufferType = AUDIO_BUFFER;
		return ms_queue_get(&obj->audioQueue);
	}
	else
	{
		uint32_t videoTimecode = mblk_get_timestamp_info(qfirst(&obj->videoQueue.q));
		uint32_t audioTimecode = mblk_get_timestamp_info(qfirst(&obj->audioQueue.q));
		if(videoTimecode <= audioTimecode)
		{
			*bufferType = VIDEO_BUFFER;
			return ms_queue_get(&obj->videoQueue);
		}
		else
		{
			*bufferType = AUDIO_BUFFER;
			return ms_queue_get(&obj->audioQueue);
		}
	}
}

/*********************************************************************************************
 * MKV Recorder Filter                                                                       *
 *********************************************************************************************/
#define CLUSTER_MAX_DURATION 5000

typedef struct
{
	Matroska file;
	const MSFmtDescriptor *vMeta;
	const MSFmtDescriptor *aMeta;
	int videoInputNum, audioInputNum;
	ModuleId videoModuleId, audioModuleId;
	void *videoModule, *audioModule;
	timecode_t duration, clusterTime, timeOffset, lastFrameTimecode;
	ms_bool_t openMode;
	MSRecorderState state;
	Muxer muxer;
	ms_bool_t needKeyFrame, firstFrame, haveVideoTrack, haveAudioTrack;
} MKVRecorder;

static void recorder_init(MSFilter *f)
{
	MKVRecorder *obj = (MKVRecorder *)ms_new(MKVRecorder, 1);

	matroska_init(&obj->file);

	obj->vMeta = NULL;
	obj->aMeta = NULL;
	obj->videoModuleId = -1;
	obj->audioModuleId = -1;
	obj->videoInputNum = -1;
	obj->audioInputNum = -1;
	obj->videoModule = NULL;
	obj->audioModule = NULL;

	obj->state = MSRecorderClosed;
	obj->needKeyFrame = TRUE;
	obj->firstFrame = TRUE;
	obj->haveVideoTrack = FALSE;
	obj->haveAudioTrack = FALSE;

	muxer_init(&obj->muxer);

	obj->timeOffset = 0;

	f->data=obj;
}

static matroska_block *write_frame(MKVRecorder *obj, mblk_t *buffer, int trackNum, ModuleId moduleId, void *module)
{
	ms_bool_t isKeyFrame;
	mblk_t *frame;

	if(modules[moduleId]->process != NULL)
		frame = modules[moduleId]->process(module, buffer, &isKeyFrame);
	else
	{
		frame = buffer;
		isKeyFrame = TRUE;
	}

	matroska_frame m_frame;
	m_frame.Timecode = ((int64_t)mblk_get_timestamp_info(frame))*1000000LL;
	m_frame.Size = msgdsize(frame);
	m_frame.Data = frame->b_rptr;

	if(matroska_clusters_count(&obj->file) == 0)
	{
		matroska_start_cluster(&obj->file, mblk_get_timestamp_info(frame));
	}
	else
	{
		if((trackNum == 1 && isKeyFrame) || (obj->duration - matroska_current_cluster_timecode(&obj->file) >= CLUSTER_MAX_DURATION))
		{
			matroska_close_cluster(&obj->file);
			matroska_start_cluster(&obj->file, mblk_get_timestamp_info(frame));
		}
	}

	matroska_block *block = matroska_write_block(&obj->file, &m_frame, trackNum, isKeyFrame);
	freemsg(frame);
	return block;
}

static void changeClockRate(mblk_t *buffer, uint32_t oldClockRate, uint32_t newClockRate)
{
	mblk_t *curBuff;
	for(curBuff = buffer; curBuff != NULL; curBuff = curBuff->b_cont)
	{
		mblk_set_timestamp_info(curBuff, mblk_get_timestamp_info(curBuff) * newClockRate / oldClockRate);
	}
}

static void moveBuffers(MSQueue *input, MSQueue *output)
{
	mblk_t *buffer;
	while((buffer = ms_queue_get(input)) != NULL)
	{
		ms_queue_put(output, buffer);
	}
}

static int recorder_open_file(MSFilter *f, void *arg)
{
	MKVRecorder *obj = (MKVRecorder *)f->data;
	const char *filename = (const char *)arg;
	int err = 0;

	ms_filter_lock(f);
	if(obj->state == MSRecorderClosed)
	{
		if (access(filename, R_OK | W_OK) == 0)
			obj->openMode = MKV_OPEN_APPEND;
		else
			obj->openMode = MKV_OPEN_CREATE;

		if(!matroska_open_file(&obj->file, filename, obj->openMode))
		{
			err = -1;
		}
		else
		{
			if(!obj->openMode)
			{
				matroska_set_doctype_version(&obj->file, MKV_DOCTYPE_VERSION, MKV_DOCTYPE_READ_VERSION);
				matroska_write_ebml_header(&obj->file);
				matroska_start_segment(&obj->file);
				matroska_write_zeros(&obj->file, 1024);
				matroska_mark_segment_info_position(&obj->file);
				matroska_write_zeros(&obj->file, 1024);

				if(obj->videoInputNum > -1 && f->inputs[obj->videoInputNum] != NULL)
				{
					matroska_add_track(&obj->file, 1, TRACK_TYPE_VIDEO, modules[obj->videoModuleId]->codecId);
					matroska_track_set_video_info(&obj->file, 1, obj->vMeta);
					if(modules[obj->videoModuleId]->set != NULL)
						modules[obj->videoModuleId]->set(obj->videoModule, obj->vMeta);
				}
				if(obj->audioInputNum > -1 && f->inputs[obj->audioInputNum] != NULL)
				{
					matroska_add_track(&obj->file, 2, TRACK_TYPE_AUDIO, modules[obj->audioModuleId]->codecId);
					matroska_track_set_audio_info(&obj->file, 2, obj->aMeta);
					if(modules[obj->audioModuleId]->set != NULL)
						modules[obj->audioModuleId]->set(obj->audioModule, obj->aMeta);
				}
				obj->duration = -1;
			}
			else
			{
				const uint8_t *data;
				size_t length;
				matroska_get_codec_private(&obj->file, 1, &data, &length);
				modules[obj->videoModuleId]->load_private_data(obj->videoModule, data);
				if(f->inputs[obj->audioInputNum] != NULL)
				{
					matroska_get_codec_private(&obj->file, 2, &data, &length);
					modules[obj->audioModuleId]->load_private_data(obj->audioModule, data);
				}
				obj->duration = matroska_get_duration(&obj->file) + 1;
			}
			obj->state = MSRecorderPaused;
		}
	}
	ms_filter_unlock(f);
	return err;
}

static int recorder_start(MSFilter *f, void *arg)
{
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int err = 0;

	ms_filter_lock(f);
	if(obj->state == MSRecorderClosed)
	{
		err = -1;
	}
	else
	{
		obj->state = MSRecorderRunning;
		obj->needKeyFrame = TRUE;
		obj->firstFrame = TRUE;
	}
	ms_filter_unlock(f);
	return err;
}

static int recorder_stop(MSFilter *f, void *arg)
{
	MKVRecorder *obj = (MKVRecorder *)f->data;
	int err;

	ms_filter_lock(f);
	if(obj->state == MSRecorderClosed)
	{
		err = -1;
	}
	else
	{
		obj->state = MSRecorderPaused;
		muxer_empty_internal_queues(&obj->muxer);
		err = 0;
	}
	ms_filter_unlock(f);
	return err;
}

static void recorder_process(MSFilter *f)
{
	MKVRecorder *obj = f->data;

	ms_filter_lock(f);
	if(obj->state == MSRecorderRunning)
	{
		mblk_t *buffer;
		MuxerBufferType bufferType;

		if(obj->videoInputNum > -1 && f->inputs[obj->videoInputNum] != NULL)
		{
			MSQueue frames, frames_ms;
			ms_queue_init(&frames);
			ms_queue_init(&frames_ms);

			if(modules[obj->videoModuleId]->preprocess != NULL)
				modules[obj->videoModuleId]->preprocess(obj->videoModule, f->inputs[obj->videoInputNum], &frames);
			else
				moveBuffers(f->inputs[obj->videoInputNum], &frames);

			while((buffer = ms_queue_get(&frames)) != NULL)
			{
				changeClockRate(buffer, obj->vMeta->rate, 1000);
				ms_queue_put(&frames_ms, buffer);
			}

			if(obj->needKeyFrame)
			{
				while((buffer = ms_queue_get(&frames_ms)) != NULL)
				{
					if(modules[obj->videoModuleId]->is_key_frame(buffer))
						break;
					else
						freemsg(buffer);
				}
				if(buffer != NULL)
				{
					muxer_put_video_buffer(&obj->muxer, buffer);
					while((buffer = ms_queue_get(&frames_ms)) != NULL)
						muxer_put_video_buffer(&obj->muxer, buffer);
					obj->needKeyFrame = FALSE;
				}
			}
			else
			{
				while((buffer = ms_queue_get(&frames_ms)) != NULL)
				{
					muxer_put_video_buffer(&obj->muxer, buffer);
				}
			}
		}

		if(obj->audioInputNum > -1 && f->inputs[obj->audioInputNum] != NULL)
		{
			MSQueue audioBuffers;
			ms_queue_init(&audioBuffers);

			if(f->inputs[obj->audioInputNum] && modules[obj->audioModuleId]->preprocess != NULL)
				modules[obj->audioModuleId]->preprocess(obj->audioModule, f->inputs[obj->audioInputNum], &audioBuffers);
			else
				moveBuffers(f->inputs[obj->audioInputNum], &audioBuffers);

			while((buffer = ms_queue_get(&audioBuffers)) != NULL)
			{
				changeClockRate(buffer, obj->aMeta->rate, 1000);
				muxer_put_audio_buffer(&obj->muxer, buffer);
			}
		}

		while((buffer = muxer_get_buffer(&obj->muxer, &bufferType)) != NULL)
		{
			if(obj->firstFrame)
			{
				obj->timeOffset = obj->duration - mblk_get_timestamp_info(buffer) + 1;
				obj->firstFrame = FALSE;
			}
			mblk_set_timestamp_info(buffer, mblk_get_timestamp_info(buffer) + obj->timeOffset);
			timecode_t bufferTimecode = mblk_get_timestamp_info(buffer);

			if(bufferType == VIDEO_BUFFER)
			{
				matroska_block *block = write_frame(obj, buffer, 1, obj->videoModuleId, obj->videoModule);
				matroska_add_cue(&obj->file, block);
				obj->haveVideoTrack = TRUE;
			}
			else
			{
				matroska_block *block = write_frame(obj, buffer, 2, obj->audioModuleId, obj->audioModule);
				if(!obj->haveVideoTrack)
				{
					matroska_add_cue(&obj->file, block);
				}
				obj->haveAudioTrack = TRUE;
			}
			if(bufferTimecode > obj->duration)
			{
				obj->duration = bufferTimecode;
			}
		}
	}
	else
	{
		if(obj->videoInputNum > -1 && f->inputs[obj->videoInputNum] != NULL)
			ms_queue_flush(f->inputs[obj->videoInputNum]);
		if(obj->audioInputNum > -1 && f->inputs[obj->audioInputNum] != NULL)
			ms_queue_flush(f->inputs[obj->audioInputNum]);
	}
	ms_filter_unlock(f);
}

static int recorder_close(MSFilter *f, void *arg)
{
	MKVRecorder *obj = (MKVRecorder *)f->data;

	ms_filter_lock(f);
	if(obj->state != MSRecorderClosed)
	{
		if(obj->videoInputNum > -1 && f->inputs[obj->videoInputNum] != NULL)
		{
			ms_queue_flush(f->inputs[obj->videoInputNum]);
			if(obj->haveVideoTrack)
			{
				uint8_t *codecPrivateData;
				size_t codecPrivateDataSize;
				modules[obj->videoModuleId]->get_private_data(obj->videoModule, &codecPrivateData, &codecPrivateDataSize);
				matroska_track_set_codec_private(&obj->file, 1, codecPrivateData, codecPrivateDataSize);
				ms_free(codecPrivateData);
			}
			else
			{
				matroska_del_track(&obj->file, 1);
			}
		}
		if(obj->audioInputNum > -1 && f->inputs[obj->audioInputNum] != NULL)
		{
			ms_queue_flush(f->inputs[obj->audioInputNum]);
			if(obj->haveAudioTrack)
			{
				uint8_t *codecPrivateData;
				size_t codecPrivateDataSize;
				modules[obj->audioModuleId]->get_private_data(obj->audioModule, &codecPrivateData, &codecPrivateDataSize);
				matroska_track_set_codec_private(&obj->file, 2, codecPrivateData, codecPrivateDataSize);
				ms_free(codecPrivateData);
			}
			else
			{
				matroska_del_track(&obj->file, 2);
			}

		}

		matroska_close_cluster(&obj->file);
		matroska_write_cues(&obj->file);

		if(!obj->openMode)
		{
			matroska_go_to_segment_info_mark(&obj->file);
		}
		else
		{
			matroska_go_to_segment_info_begin(&obj->file);
		}
		matroska_set_segment_info(&obj->file, "libmediastreamer2", "libmediastreamer2", obj->duration);
		matroska_write_segment_info(&obj->file);
		matroska_write_tracks(&obj->file);
		matroska_go_to_segment_begin(&obj->file);
		matroska_write_metaSeek(&obj->file);
		matroska_go_to_file_end(&obj->file);
		matroska_close_segment(&obj->file);
		matroska_close_file(&obj->file);

		obj->state = MSRecorderClosed;
	}
	ms_filter_unlock(f);

	return 0;
}

static void recorder_uninit(MSFilter *f){
	MKVRecorder *obj = (MKVRecorder *)f->data;
	muxer_uninit(&obj->muxer);
	if(obj->state != MSRecorderClosed)
	{
		matroska_close_file(&obj->file);
	}
	ms_free(obj);
}

static int recorder_set_input_fmt(MSFilter *f, void *arg)
{
	MKVRecorder *data=(MKVRecorder *)f->data;
	const MSPinFormat *pinFmt = (const MSPinFormat *)arg;

	switch(pinFmt->fmt->type)
	{
	case MSAudio:
		if(data->audioModule != NULL)
		{
			modules[data->audioModuleId]->uninit(data->audioModule);
			data->audioModule = NULL;
		}
		data->audioModuleId = find_module_id(pinFmt->fmt->encoding);
		if(data->audioModuleId == -1)
		{
			return -2;
		}
		else
		{
			data->aMeta = pinFmt->fmt;
			data->audioInputNum = pinFmt->pin;
			modules[data->audioModuleId]->init(&data->audioModule);
			return 0;
		}
		break;

	case MSVideo:
		if(data->videoModule != NULL)
		{
			modules[data->videoModuleId]->uninit(data->videoModule);
			data->videoModule = NULL;
		}
		data->videoModuleId = find_module_id(pinFmt->fmt->encoding);
		if(data->videoModuleId == -1)
		{
			return -2;
		}
		else
		{
			data->vMeta = pinFmt->fmt;
			data->videoInputNum = pinFmt->pin;
			modules[data->videoModuleId]->init(&data->videoModule);
			return 0;
		}
		break;

	default:
		return -1;
	}
}

static MSFilterMethod recorder_methods[]= {
	{	MS_RECORDER_OPEN            ,	recorder_open_file         },
	{	MS_RECORDER_CLOSE           ,	recorder_close             },
	{	MS_RECORDER_START           ,	recorder_start             },
	{	MS_RECORDER_PAUSE           ,	recorder_stop              },
	{   MS_FILTER_SET_INPUT_FMT     ,   recorder_set_input_fmt     },
	{   0                           ,   NULL                       }
};

#ifdef _MSC_VER
MSFilterDesc ms_mkv_recorder_desc= {
	MS_MKV_RECORDER_ID,
	"MSMKVRecorder",
	"MKV file recorder",
	MS_FILTER_OTHER,
	2,
	0,
	recorder_init,
	NULL,
	recorder_process,
	NULL,
	recorder_uninit,
	recorder_methods
};
#else
MSFilterDesc ms_mkv_recorder_desc={
	.id=MS_MKV_RECORDER_ID,
	.name="MSMKVRecorder",
	.text="MKV file recorder",
	.category=MS_FILTER_OTHER,
	.ninputs=2,
	.noutputs=0,
	.init=recorder_init,
	.preprocess=NULL,
	.process=recorder_process,
	.postprocess=NULL,
	.uninit=recorder_uninit,
	.methods=recorder_methods
};
#endif

MS_FILTER_DESC_EXPORT(ms_mkv_recorder_desc)


/*********************************************************************************************
 * MKV Player Filter                                                                         *
 *********************************************************************************************/
typedef struct
{
	Matroska file;
	MSPlayerState state;
	void *videoModule, *audioModule;
	ModuleId videoModuleId, audioModuleId;
	int videoPin, audioPin;
	timecode_t time;
} MKVPlayer;

static void player_init(MSFilter *f)
{
	MKVPlayer *obj = (MKVPlayer *)ms_new(MKVPlayer, 1);
	matroska_init(&obj->file);
	obj->state = MSPlayerClosed;
	obj->videoModule = NULL, obj->audioModule = NULL;
	obj->videoModuleId = -1, obj->audioModuleId = -1;
	obj->videoPin = 0, obj->audioPin = 1;
	obj->time = 0;
	f->data = obj;
}

static void player_process(MSFilter *f)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	obj->time += f->ticker->interval;
	if(obj->videoModuleId > -1 && f->inputs[obj->videoPin])
	{

	}
	ms_filter_unlock(f);
}

static void player_uninit(MSFilter *f)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	if(obj->state != MSPlayerClosed)
	{
		matroska_close_file(&obj->file);
	}
	ms_free(obj);
}

static int player_open_file(MSFilter *f, void *arg)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	const char *filename = (const char *)arg;
	int err = 0;
	ms_filter_lock(f);
	if(obj->state != MSPlayerClosed)
	{
		err = -1;
	}
	else
	{
		if(matroska_open_file(&obj->file, filename, MKV_OPEN_RO) != -1)
		{
			err = -2;
		}
		else
		{
			int trackNum;
			char codecId[256];
			if((trackNum = matroska_get_default_track_num(&obj->file, TRACK_TYPE_VIDEO)) != -1)
			{
				if(matroska_track_get_codec_id(&obj->file, trackNum, codecId, 256) == 0)
				{
					obj->videoModuleId = find_module_id_by_codec_id(codecId);
					if(obj->videoModuleId > -1)
					{
						modules[obj->videoModuleId]->init(&obj->videoModule);
					}
				}
			}
			if((trackNum = matroska_get_default_track_num(&obj->file, TRACK_TYPE_AUDIO)) != -1)
			{
				if(matroska_track_get_codec_id(&obj->file, trackNum, codecId, 256) == 0)
				{
					obj->audioModuleId = find_module_id_by_codec_id(codecId);
					if(obj->audioModuleId > -1)
					{
						modules[obj->audioModuleId]->init(&obj->audioModule);
					}
				}
			}

			obj->state = MSPlayerPaused;
		}
	}
	ms_filter_unlock(f);
	return err;
}

static int player_close(MSFilter *f, void *arg)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	if(obj->state != MSPlayerClosed)
	{
		matroska_close_file(&obj->file);
		if(obj->videoModule != NULL)
		{
			modules[obj->videoModuleId]->uninit(obj->videoModule);
			obj->videoModuleId = -1;
			obj->videoModule = NULL;
		}
		if(obj->audioModule != NULL)
		{
			modules[obj->audioModuleId]->uninit(obj->audioModule);
			obj->audioModuleId = -1;
			obj->audioModule = NULL;
		}
		obj->state = MSPlayerClosed;
	}
	ms_filter_unlock(f);
	return 0;
}

static int player_start(MSFilter *f, void *arg)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	int err = 0;
	ms_filter_lock(f);
	if(obj->state == MSPlayerClosed)
	{
		err = -1;
	}
	else
	{
		obj->state = MSPlayerPlaying;
	}
	ms_filter_unlock(f);
	return err;
}

static int player_stop(MSFilter *f, void *arg)
{
	MKVPlayer *obj = (MKVPlayer *)f->data;
	ms_filter_lock(f);
	if(obj->state == MSPlayerPlaying)
	{
		obj->state = MSPlayerPaused;
	}
	ms_filter_unlock(f);
	return 0;
}

static MSFilterMethod player_methods[]= {
	{	MS_PLAYER_OPEN		,	player_open_file	},
	{	MS_PLAYER_CLOSE		,	player_close		},
	{	MS_PLAYER_START		,	player_start		},
	{	MS_PLAYER_PAUSE		,	player_stop			},
	{	0					,	NULL				}
};

#ifdef _MSC_VER
MSFilterDesc ms_mkv_player_desc= {
	MS_MKV_PLAYER_ID,
	"MSMKVPlayer",
	"MKV file player",
	MS_FILTER_OTHER,
	2,
	0,
	player_init,
	NULL,
	player_process,
	NULL,
	player_uninit,
	player_methods
};
#else
MSFilterDesc ms_mkv_player_desc={
	.id=MS_MKV_PLAYER_ID,
	.name="MSMKVPlayer",
	.text="MKV file player",
	.category=MS_FILTER_OTHER,
	.ninputs=2,
	.noutputs=0,
	.init=player_init,
	.preprocess=NULL,
	.process=player_process,
	.postprocess=NULL,
	.uninit=player_uninit,
	.methods=player_methods
};
#endif

MS_FILTER_DESC_EXPORT(ms_mkv_player_desc)
