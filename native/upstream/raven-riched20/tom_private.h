/* SPDX-License-Identifier: MIT */
#ifndef RAVEN_RICHED20_TOM_PRIVATE_H
#define RAVEN_RICHED20_TOM_PRIVATE_H

#include "re.h"

#define RE_OBJECT RE_OBJECT_MARK

extern const IID IID_ITextDocument2Old_;

typedef struct text_range text_range;

typedef struct rich_ole
{
	IUnknown inner;
	IUnknown *controlling;		// who answers for the identity and the lifetime of the interfaces
	IRichEditOle ole;
	ITextDocument document;
	LONG refs;
	re_editor *editor;
	REOBJECT *objects;		// in the order of their marks in the text
	INT object_count;
	text_range *ranges;		// every live range, which follow the text as it changes
	text_range *selection;
	struct client_site *sites;
	LONG frozen;
} rich_ole;

// What a range and a selection share; a selection keeps its ends in the editor.
struct text_range
{
	ITextRange iface;
	LONG refs;
	rich_ole *owner;
	LONG start, end;
	BOOL is_selection;
	text_range *next;
};

extern const ITextDocumentVtbl tom_document_vtbl;

HRESULT tom_client_site_create(rich_ole *owner, IOleClientSite **out);
void tom_detach(rich_ole *self);
void tom_ranges_shift(rich_ole *self, INT at, INT removed, INT inserted);
void tom_ranges_reset(rich_ole *self);
HRESULT tom_range_create(rich_ole *owner, LONG start, LONG end, ITextRange **out);
HRESULT tom_selection_get(rich_ole *owner, ITextSelection **out);
HRESULT tom_font_create(text_range *range, ITextFont **out);
HRESULT tom_para_create(text_range *range, ITextPara **out);
void tom_range_ends(const text_range *range, LONG *start, LONG *end);

// Automation goes through the type library the module carries.
HRESULT tom_type_info(REFIID iid, ITypeInfo **out);
HRESULT tom_get_ids(REFIID iid, LPOLESTR *names, UINT count, DISPID *ids);
HRESULT tom_invoke(void *object, REFIID iid, DISPID id, WORD flags, DISPPARAMS *params, VARIANT *result, EXCEPINFO *exception, UINT *error);

#endif
