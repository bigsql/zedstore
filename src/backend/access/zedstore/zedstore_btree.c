/*
 * zedstore_btree.c
 *		Routines for handling B-trees structures in ZedStore
 *
 * A Zedstore table consists of multiple B-trees, one for each attribute. The
 * functions in this file deal with one B-tree at a time, it is the caller's
 * responsibility to tie together the scans of each btree.
 *
 * Operations:
 *
 * - Sequential scan in TID order
 *  - must be efficient with scanning multiple trees in sync
 *
 * - random lookups, by TID (for index scan)
 *
 * - range scans by TID (for bitmap index scan)
 *
 * NOTES:
 * - Locking order: child before parent, left before right
 *
 * Portions Copyright (c) 1996-2019, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 * IDENTIFICATION
 *	  src/backend/access/zedstore/zedstore_btree.c
 */
#include "postgres.h"

#include "access/tableam.h"
#include "access/xact.h"
#include "access/zedstore_compression.h"
#include "access/zedstore_internal.h"
#include "access/zedstore_undo.h"
#include "miscadmin.h"
#include "storage/bufmgr.h"
#include "storage/procarray.h"
#include "utils/datum.h"
#include "utils/rel.h"

/* prototypes for local functions */
static Buffer zsbt_descend(Relation rel, BlockNumber rootblk, zstid key);
static Buffer zsbt_find_downlink(Relation rel, AttrNumber attno,
								 zstid key, BlockNumber childblk, int level,
								 int *itemno);
static void zsbt_recompress_replace(Relation rel, AttrNumber attno,
									Buffer oldbuf, List *items);
static void zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf,
								 zstid rightlokey, BlockNumber rightblkno);
static void zsbt_split_internal_page(Relation rel, AttrNumber attno,
									 Buffer leftbuf, Buffer childbuf,
									 OffsetNumber newoff, zstid newkey, BlockNumber childblk);
static void zsbt_newroot(Relation rel, AttrNumber attno, int level,
						 zstid key1, BlockNumber blk1,
						 zstid key2, BlockNumber blk2,
						 Buffer leftchildbuf);
static ZSSingleBtreeItem *zsbt_fetch(Relation rel, AttrNumber attno, Snapshot snapshot,
		   zstid tid, Buffer *buf_p);
static void zsbt_replace_item(Relation rel, AttrNumber attno, Buffer buf,
							  zstid oldtid, ZSBtreeItem *replacementitem,
							  List *newitems);

static int zsbt_binsrch_internal(zstid key, ZSBtreeInternalPageItem *arr, int arr_elems);

static TM_Result zsbt_update_lock_old(Relation rel, AttrNumber attno, zstid otid,
					 TransactionId xid, CommandId cid, Snapshot snapshot,
					 Snapshot crosscheck, bool wait, TM_FailureData *hufd);
static void zsbt_update_insert_new(Relation rel, AttrNumber attno,
					   Datum newdatum, bool newisnull, zstid *newtid,
					   TransactionId xid, CommandId cid);
static void zsbt_mark_old_updated(Relation rel, AttrNumber attno, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, Snapshot snapshot);

/* ----------------------------------------------------------------
 *						 Public interface
 * ----------------------------------------------------------------
 */

/*
 * Begin a scan of the btree.
 */
void
zsbt_begin_scan(Relation rel, AttrNumber attno, zstid starttid, Snapshot snapshot, ZSBtreeScan *scan)
{
	BlockNumber	rootblk;
	int16		attlen;
	bool		attbyval;
	Buffer		buf;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, false, &attlen, &attbyval);

	if (rootblk == InvalidBlockNumber)
	{
		/* completely empty tree */
		scan->rel = NULL;
		scan->attno = InvalidAttrNumber;
		scan->attlen = 0;
		scan->attbyval = false;
		scan->active = false;
		scan->lastbuf = InvalidBuffer;
		scan->lastbuf_is_locked = false;
		scan->lastoff = InvalidOffsetNumber;
		scan->snapshot = NULL;
		scan->context = NULL;
		memset(&scan->recent_oldest_undo, 0, sizeof(scan->recent_oldest_undo));
		scan->nexttid = InvalidZSTid;
		scan->array_item = NULL;
		scan->array_elements_left = 0;
		return;
	}

	buf = zsbt_descend(rel, rootblk, starttid);
	LockBuffer(buf, BUFFER_LOCK_UNLOCK);

	scan->rel = rel;
	scan->attno = attno;
	scan->attlen = attlen;
	scan->attbyval = attbyval;
	scan->snapshot = snapshot;

	scan->active = true;
	scan->lastbuf = buf;
	scan->lastbuf_is_locked = false;
	scan->lastoff = InvalidOffsetNumber;
	scan->nexttid = starttid;

	scan->context = CurrentMemoryContext;

	scan->has_decompressed = false;
	zs_decompress_init(&scan->decompressor);
	scan->array_item = NULL;
	scan->array_elements_left = 0;

	memset(&scan->recent_oldest_undo, 0, sizeof(scan->recent_oldest_undo));
}

void
zsbt_end_scan(ZSBtreeScan *scan)
{
	if (!scan->active)
		return;

	if (scan->lastbuf != InvalidBuffer)
	{
		if (scan->lastbuf_is_locked)
			LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
		ReleaseBuffer(scan->lastbuf);
	}
	zs_decompress_free(&scan->decompressor);

	scan->active = false;
}

/*
 * Return true if there was another tuple. The datum is returned in *datum,
 * and its TID in *tid. For a pass-by-ref datum, it's a palloc'd copy.
 */
bool
zsbt_scan_next(ZSBtreeScan *scan, Datum *datum, bool *isnull, zstid *tid)
{
	Buffer		buf;
	bool		buf_is_locked = false;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber off;
	OffsetNumber maxoff;
	BlockNumber	next;

	if (!scan->active)
		return false;

	/*
	 * Process items, until we find something that is visible to the snapshot.
	 *
	 * This advances scan->nexttid as it goes.
	 */
	for (;;)
	{
		/*
		 * If we are still processing an array item, return next element from it.
		 */
		if (scan->array_elements_left > 0)
		{
			if (scan->array_isnull)
				*isnull = true;
			else
			{
				char	   *dataptr = scan->array_next_datum;

				*datum = fetch_att(dataptr, scan->attbyval, scan->attlen);

				/* make a copy, to make sure it's aligned. */
				if (scan->attlen < 0 && !VARATT_IS_1B(*datum))
					*datum = zs_datumCopy(*datum, scan->attbyval, scan->attlen);

				*isnull = false;
				if (scan->attlen > 0)
					dataptr += scan->attlen;
				else
				{
					dataptr += zs_datumGetSize(PointerGetDatum(dataptr), scan->attbyval, scan->attlen);
				}
				scan->array_next_datum = dataptr;
			}
			*tid = scan->array_next_tid;
			scan->array_next_tid++;
			scan->nexttid = scan->array_next_tid;
			scan->array_elements_left--;
			return true;
		}

		/*
		 * If we are still processing a compressed item, process the next item
		 * from the it. If it's an array item, we start iterating the array by
		 * setting the scan->array_* fields, and loop back to top to return the
		 * first element from the array.
		 */
		if (scan->has_decompressed)
		{
			zstid		lasttid;
			ZSBtreeItem *uitem;

			uitem = zs_decompress_read_item(&scan->decompressor);

			if (uitem == NULL)
			{
				scan->has_decompressed = false;
				continue;
			}

			/* a compressed item cannot contain nested compressed items */
			Assert((uitem->t_flags & ZSBT_COMPRESSED) == 0);

			lasttid = zsbt_item_lasttid(uitem);
			if (lasttid < scan->nexttid)
				continue;

			if (!zs_SatisfiesVisibility(scan, uitem))
			{
				scan->nexttid = lasttid + 1;
				continue;
			}
			if ((uitem->t_flags & ZSBT_ARRAY) != 0)
			{
				/* no need to make a copy, because the uncompressed buffer
				 * is already a copy */
				ZSArrayBtreeItem *aitem = (ZSArrayBtreeItem *) uitem;

				scan->array_item = aitem;
				scan->array_isnull = (aitem->t_flags & ZSBT_NULL) != 0;
				scan->array_next_datum = aitem->t_payload;
				scan->array_next_tid = aitem->t_tid;
				scan->array_elements_left = aitem->t_nelements;

				while (scan->array_next_tid < scan->nexttid && scan->array_elements_left > 0)
				{
					if (scan->attlen > 0)
						scan->array_next_datum += scan->attlen;
					else
						scan->array_next_datum += zs_datumGetSize(PointerGetDatum(scan->array_next_datum), scan->attbyval, scan->attlen);

					scan->array_next_tid++;
					scan->array_elements_left--;
				}
				scan->nexttid = scan->array_next_tid;
				continue;
			}
			else
			{
				/* single item */
				ZSSingleBtreeItem *sitem = (ZSSingleBtreeItem *) uitem;

				*tid = sitem->t_tid;
				if (sitem->t_flags & ZSBT_NULL)
					*isnull = true;
				else
				{
					*isnull = false;
					*datum = fetch_att(sitem->t_payload, scan->attbyval, scan->attlen);
					/* no need to copy, because the uncompression buffer is a copy already */
					/* FIXME: do we need to copy anyway, to make sure it's aligned correctly? */
				}
				scan->nexttid = sitem->t_tid + 1;

				if (buf_is_locked)
					LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
				buf_is_locked = false;
				return true;
			}
		}

		/*
		 * Scan the page for the next item.
		 */
		buf = scan->lastbuf;
		if (!buf_is_locked)
		{
			LockBuffer(buf, BUFFER_LOCK_SHARE);
			buf_is_locked = true;
		}
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);
		Assert(opaque->zs_page_id == ZS_BTREE_PAGE_ID);

		/* TODO: check the last offset first, as an optimization */
		maxoff = PageGetMaxOffsetNumber(page);
		for (off = FirstOffsetNumber; off <= maxoff; off++)
		{
			ItemId		iid = PageGetItemId(page, off);
			ZSBtreeItem	*item = (ZSBtreeItem *) PageGetItem(page, iid);
			zstid		lasttid;

			lasttid = zsbt_item_lasttid(item);

			if (scan->nexttid > lasttid)
				continue;

			if ((item->t_flags & ZSBT_COMPRESSED) != 0)
			{
				ZSCompressedBtreeItem *citem = (ZSCompressedBtreeItem *) item;
				MemoryContext oldcxt = MemoryContextSwitchTo(scan->context);

				zs_decompress_chunk(&scan->decompressor, citem);
				MemoryContextSwitchTo(oldcxt);
				scan->has_decompressed = true;
				LockBuffer(buf, BUFFER_LOCK_UNLOCK);
				buf_is_locked = false;
				break;
			}
			else
			{
				if (!zs_SatisfiesVisibility(scan, item))
				{
					scan->nexttid = lasttid + 1;
					continue;
				}

				if ((item->t_flags & ZSBT_ARRAY) != 0)
				{
					/* copy the item, because we can't hold a lock on the page  */
					ZSArrayBtreeItem *aitem;

					aitem = MemoryContextAlloc(scan->context, item->t_size);
					memcpy(aitem, item, item->t_size);

					scan->array_item = aitem;
					scan->array_next_datum = aitem->t_payload;
					scan->array_next_tid = aitem->t_tid;
					scan->array_elements_left = aitem->t_nelements;

					while (scan->array_next_tid < scan->nexttid && scan->array_elements_left > 0)
					{
						if (scan->attlen > 0)
							scan->array_next_datum += scan->attlen;
						else
							scan->array_next_datum += zs_datumGetSize(PointerGetDatum(scan->array_next_datum), scan->attbyval, scan->attlen);

						scan->array_next_tid++;
						scan->array_elements_left--;
					}
					scan->nexttid = scan->array_next_tid;

					if (scan->array_elements_left > 0)
					{
						LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
						buf_is_locked = false;
						break;
					}
				}
				else
				{
					/* single item */
					ZSSingleBtreeItem *sitem = (ZSSingleBtreeItem *) item;

					*tid = item->t_tid;
					if (item->t_flags & ZSBT_NULL)
						*isnull = true;
					else
					{
						*isnull = false;
						*datum = fetch_att(sitem->t_payload, scan->attbyval, scan->attlen);
						*datum = zs_datumCopy(*datum, scan->attbyval, scan->attlen);
					}
					scan->nexttid = sitem->t_tid + 1;
					LockBuffer(scan->lastbuf, BUFFER_LOCK_UNLOCK);
					buf_is_locked = false;
					return true;
				}
			}
		}

		if (scan->array_elements_left > 0 || scan->has_decompressed)
			continue;

		/* No more items on this page. Walk right, if possible */
		next = opaque->zs_next;
		if (next == BufferGetBlockNumber(buf))
			elog(ERROR, "btree page %u next-pointer points to itself", next);
		LockBuffer(buf, BUFFER_LOCK_UNLOCK);
		buf_is_locked = false;

		if (next == InvalidBlockNumber)
		{
			scan->active = false;
			ReleaseBuffer(scan->lastbuf);
			scan->lastbuf = InvalidBuffer;
			break;
		}

		scan->lastbuf = ReleaseAndReadBuffer(scan->lastbuf, scan->rel, next);
	}

	return false;
}

/*
 * Get the last tid (plus one) in the tree.
 */
zstid
zsbt_get_last_tid(Relation rel, AttrNumber attno)
{
	BlockNumber	rootblk;
	zstid		rightmostkey;
	zstid		tid;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber maxoff;
	int16		attlen;
	bool		attbyval;

	/* Find the rightmost leaf */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);
	rightmostkey = MaxZSTid;
	buf = zsbt_descend(rel, rootblk, rightmostkey);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);

	/*
	 * Look at the last item, for its tid.
	 */
	maxoff = PageGetMaxOffsetNumber(page);
	if (maxoff >= FirstOffsetNumber)
	{
		ItemId		iid = PageGetItemId(page, maxoff);
		ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

		tid = zsbt_item_lasttid(hitup) + 1;
	}
	else
	{
		tid = opaque->zs_lokey;
	}
	UnlockReleaseBuffer(buf);

	return tid;
}

/*
 * Compute the size of a slice of an array, from an array item. 'dataptr'
 * points to the packed on-disk representation of the array item's data.
 * The elements are stored one after each other.
 */
static Size
zsbt_get_array_slice_len(int16 attlen, bool attbyval, bool isnull,
						 char *dataptr, int nelements)
{
	Size		datasz;

	if (isnull)
		datasz = 0;
	else
	{
		/*
		 * For a fixed-width type, we can just multiply. For variable-length,
		 * we have to walk through the elements, looking at the length of each
		 * element.
		 */
		if (attlen > 0)
		{
			datasz = attlen * nelements;
		}
		else
		{
			char	   *p = dataptr;
			Size		datumsz = 0;

			datasz = 0;
			for (int i = 0; i < nelements; i++)
			{
				datumsz = zs_datumGetSize(PointerGetDatum(p), attbyval, attlen);

				/*
				 * The array should already use short varlen representation whenever
				 * possible.
				 */
				Assert(!VARATT_CAN_MAKE_SHORT(DatumGetPointer(p)));

				datasz += datumsz;
				p += datumsz;
			}
		}
	}
	return datasz;
}

static ZSBtreeItem *
zsbt_create_item(int16 attlen, bool attbyval, zstid tid, ZSUndoRecPtr undo_ptr,
				 int nelements,
				 Datum *datums,
				 char *dataptr, Size datasz, bool isnull)
{
	ZSBtreeItem *result;
	Size		itemsz;
	char	   *databegin;

	Assert(nelements > 0);

	/*
	 * Form a ZSBtreeItem to insert.
	 */
	if (nelements > 1)
	{
		ZSArrayBtreeItem *newitem;

		itemsz = offsetof(ZSArrayBtreeItem, t_payload) + datasz;

		newitem = palloc(itemsz);
		memset(newitem, 0, offsetof(ZSArrayBtreeItem, t_payload)); /* zero padding */
		newitem->t_tid = tid;
		newitem->t_size = itemsz;
		newitem->t_flags = ZSBT_ARRAY;
		if (isnull)
			newitem->t_flags |= ZSBT_NULL;
		newitem->t_nelements = nelements;
		newitem->t_undo_ptr = undo_ptr;

		databegin = newitem->t_payload;

		result = (ZSBtreeItem *) newitem;
	}
	else
	{
		ZSSingleBtreeItem *newitem;

		itemsz = offsetof(ZSSingleBtreeItem, t_payload) + datasz;

		newitem = palloc(itemsz);
		memset(newitem, 0, offsetof(ZSSingleBtreeItem, t_payload)); /* zero padding */
		newitem->t_tid = tid;
		newitem->t_flags = 0;
		if (isnull)
			newitem->t_flags |= ZSBT_NULL;
		newitem->t_size = itemsz;
		newitem->t_undo_ptr = undo_ptr;

		databegin = newitem->t_payload;

		result = (ZSBtreeItem *) newitem;
	}

	if (!isnull)
	{
		char	   *datadst = databegin;

		if (datums)
		{
			for (int i = 0; i < nelements; i++)
			{
				Datum		val = datums[i];

				if (attbyval)
				{
					store_att_byval(datadst, val, attlen);
					datadst += attlen;
				}
				else
				{
					if (attlen == -1 && VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
					{
						/* convert to short varlena */
						Size		data_length = VARATT_CONVERTED_SHORT_SIZE(val);

						SET_VARSIZE_SHORT(datadst, data_length);
						memcpy(datadst + 1, VARDATA(val), data_length - 1);
						datadst += data_length;
					}
					else
					{
						/* full 4-byte header varlena, or was already short */
						Size		datumsz = zs_datumGetSize(PointerGetDatum(val), attbyval, attlen);

						memcpy(datadst, DatumGetPointer(val), datumsz);
						datadst += datumsz;
					}
				}
			}
			Assert(datadst - databegin == datasz);
		}
		else
			memcpy(datadst, dataptr, datasz);
	}

	return result;
}


/*
 * Insert a multiple items to the given attribute's btree.
 *
 * Populates the TIDs of the new tuples.
 *
 * If 'tid' in list is valid, then that TID is used. It better not be in use already. If
 * it's invalid, then a new TID is allocated, as we see best. (When inserting the
 * first column of the row, pass invalid, and for other columns, pass the TID
 * you got for the first column.)
 */
void
zsbt_multi_insert(Relation rel, AttrNumber attno,
				  Datum *datums, bool *isnulls, zstid *tids, int nitems,
				  TransactionId xid, CommandId cid, ZSUndoRecPtr *undorecptr)
{
	Form_pg_attribute attr = &rel->rd_att->attrs[attno - 1];
	int16		attlen;
	bool		attbyval;
	bool		assign_tids;
	zstid		tid = tids[0];
	BlockNumber	rootblk;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	OffsetNumber maxoff;
	zstid		insert_target_key;
	ZSUndoRec_Insert undorec;
	int			i;
	List	   *newitems;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);

	if (attr->attbyval != attbyval || attr->attlen != attlen)
		elog(ERROR, "attribute information stored in root dir doesn't match with rel");

	/*
	 * If TID was given, find the right place for it. Otherwise, insert to
	 * the rightmost leaf.
	 *
	 * TODO: use a Free Space Map to find suitable target.
	 */
	assign_tids = (tid == InvalidZSTid);

	if (!assign_tids)
		insert_target_key = tid;
	else
		insert_target_key = MaxZSTid;

	buf = zsbt_descend(rel, rootblk, insert_target_key);
	page = BufferGetPage(buf);
	opaque = ZSBtreePageGetOpaque(page);
	maxoff = PageGetMaxOffsetNumber(page);

	/*
	 * Look at the last item, for its tid.
	 *
	 * assign TIDS for each item, if needed.
	 */
	if (assign_tids)
	{
		zstid		lasttid;

		if (maxoff >= FirstOffsetNumber)
		{
			ItemId		iid = PageGetItemId(page, maxoff);
			ZSBtreeItem	*hitup = (ZSBtreeItem *) PageGetItem(page, iid);

			lasttid = zsbt_item_lasttid(hitup);
			tid = lasttid + 1;
		}
		else
		{
			lasttid = opaque->zs_lokey;
			tid = lasttid;
		}

		for (i = 0; i < nitems; i++)
		{
			tids[i] = tid;
			tid++;
		}
	}

	/* Form an undo record */
	if (!IsZSUndoRecPtrValid(undorecptr))
	{
		undorec.rec.size = sizeof(ZSUndoRec_Insert);
		undorec.rec.type = ZSUNDO_TYPE_INSERT;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tids[0];
		undorec.endtid = tids[nitems - 1];
		*undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Create items to insert */
	newitems = NIL;
	i = 0;
	while (i < nitems)
	{
		Size		datasz;
		int			j;
		ZSBtreeItem *newitem;

		datasz = 0;
		if (!isnulls[i])
		{
			Datum		val = datums[i];

			if (attlen == -1 && VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
			{
				/* will be converted to short varlena */
				Size		data_length = VARATT_CONVERTED_SHORT_SIZE(val);

				datasz += data_length;
			}
			else
				datasz += zs_datumGetSize(datums[i], attbyval, attlen);
		}
		for (j = i + 1; j < nitems && datasz < MaxZedStoreDatumSize / 4; j++)
		{
			if (isnulls[j] != isnulls[i])
				break;

			if (tids[j] != tids[j - 1] + 1)
				break;

			if (!isnulls[i])
			{
				Datum		val = datums[j];

				if (attlen == -1 && VARATT_CAN_MAKE_SHORT(DatumGetPointer(val)))
				{
					/* will be converted to short varlena */
					Size		data_length = VARATT_CONVERTED_SHORT_SIZE(val);

					datasz += data_length;
				}
				else
					datasz += zs_datumGetSize(datums[j], attbyval, attlen);
			}
		}

		newitem = zsbt_create_item(attlen, attbyval, tids[i], *undorecptr,
								   j - i, &datums[i], NULL, datasz, isnulls[i]);

		newitems = lappend(newitems, newitem);
		i = j;
	}

	/* recompress and possibly split the page */
	zsbt_replace_item(rel, attno, buf,
					  InvalidZSTid, NULL,
					  newitems);
	/* zsbt_replace_item unlocked 'buf' */
	ReleaseBuffer(buf);
}

TM_Result
zsbt_delete(Relation rel, AttrNumber attno, zstid tid,
			TransactionId xid, CommandId cid,
			Snapshot snapshot, Snapshot crosscheck, bool wait,
			TM_FailureData *hufd, bool changingPart)
{
	ZSSingleBtreeItem *item;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSSingleBtreeItem *deleteditem;
	Buffer		buf;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_fetch(rel, attno, snapshot, tid, &buf);
	if (item == NULL)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to delete with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
	}
	result = zs_SatisfiesUpdate(rel, snapshot, (ZSBtreeItem *) item, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Delete undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Delete);
		undorec.rec.type = ZSUNDO_TYPE_DELETE;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;

		if (keep_old_undo_ptr)
			undorec.prevundorec = item->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with a DELETED item. */
	deleteditem = palloc(item->t_size);
	memcpy(deleteditem, item, item->t_size);
	deleteditem->t_flags |= ZSBT_DELETED;
	deleteditem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, buf,
					  item->t_tid, (ZSBtreeItem *) deleteditem,
					  NIL);
	ReleaseBuffer(buf);	/* zsbt_replace_item unlocked */

	pfree(deleteditem);

	return TM_Ok;
}

/*
 * If 'newtid' is valid, then that TID is used for the new item. It better not
 * be in use already. If it's invalid, then a new TID is allocated, as we see
 * best. (When inserting the first column of the row, pass invalid, and for
 * other columns, pass the TID you got for the first column.)
 */
TM_Result
zsbt_update(Relation rel, AttrNumber attno, zstid otid, Datum newdatum,
			bool newisnull, TransactionId xid, CommandId cid, Snapshot snapshot,
			Snapshot crosscheck, bool wait, TM_FailureData *hufd,
			zstid *newtid_p)
{
	TM_Result	result;

	/*
	 * Find and lock the old item.
	 *
	 * TODO: If there's free TID space left on the same page, we should keep the
	 * buffer locked, and use the same page for the new tuple.
	 */
	result = zsbt_update_lock_old(rel, attno, otid,
								  xid, cid, snapshot,
								  crosscheck, wait, hufd);

	if (result != TM_Ok)
		return result;

	/* insert new version */
	zsbt_update_insert_new(rel, attno, newdatum, newisnull, newtid_p, xid, cid);

	/* update the old item with the "t_ctid pointer" for the new item */
	zsbt_mark_old_updated(rel, attno, otid, *newtid_p, xid, cid, snapshot);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): locks the old item for update.
 */
static TM_Result
zsbt_update_lock_old(Relation rel, AttrNumber attno, zstid otid,
					 TransactionId xid, CommandId cid, Snapshot snapshot,
					 Snapshot crosscheck, bool wait, TM_FailureData *hufd)
{
	Buffer		buf;
	ZSSingleBtreeItem *olditem;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;

	/*
	 * Find the item to delete.
	 */
	olditem = zsbt_fetch(rel, attno, snapshot, otid, &buf);
	if (olditem == NULL)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid), attno);
	}

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(rel, snapshot, (ZSBtreeItem *) olditem, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	/*
	 * TODO: tuple-locking not implemented. Pray that there is no competing
	 * concurrent update!
	 */

	UnlockReleaseBuffer(buf);

	return TM_Ok;
}

/*
 * Subroutine of zsbt_update(): inserts the new, updated, item.
 */
static void
zsbt_update_insert_new(Relation rel, AttrNumber attno,
					   Datum newdatum, bool newisnull, zstid *newtid,
					   TransactionId xid, CommandId cid)
{
	ZSUndoRecPtr undorecptr;

	ZSUndoRecPtrInitialize(&undorecptr);
	zsbt_multi_insert(rel, attno, &newdatum, &newisnull, newtid, 1,
					  xid, cid, &undorecptr);
}

/*
 * Subroutine of zsbt_update(): mark old item as updated.
 */
static void
zsbt_mark_old_updated(Relation rel, AttrNumber attno, zstid otid, zstid newtid,
					  TransactionId xid, CommandId cid, Snapshot snapshot)
{
	Buffer		buf;
	ZSSingleBtreeItem *olditem;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	TM_FailureData tmfd;
	ZSUndoRecPtr undorecptr;
	ZSSingleBtreeItem *deleteditem;

	/*
	 * Find the item to delete.  It could be part of a compressed item,
	 * we let zsbt_fetch() handle that.
	 */
	olditem = zsbt_fetch(rel, attno, snapshot, otid, &buf);
	if (olditem == NULL)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find old tuple to update with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(otid), ZSTidGetOffsetNumber(otid), attno);
	}

	/*
	 * Is it visible to us?
	 */
	result = zs_SatisfiesUpdate(rel, snapshot, (ZSBtreeItem *) olditem, &keep_old_undo_ptr, &tmfd);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		elog(ERROR, "tuple concurrently updated - not implemented");
	}

	/* Create UNDO record. */
	{
		ZSUndoRec_Update undorec;

		undorec.rec.size = sizeof(ZSUndoRec_Update);
		undorec.rec.type = ZSUNDO_TYPE_UPDATE;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = otid;
		if (keep_old_undo_ptr)
			undorec.prevundorec = olditem->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);
		undorec.newtid = newtid;

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the ZSBreeItem with an UPDATED item. */
	deleteditem = palloc(olditem->t_size);
	memcpy(deleteditem, olditem, olditem->t_size);
	deleteditem->t_flags |= ZSBT_UPDATED;
	deleteditem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, buf,
					  otid, (ZSBtreeItem *) deleteditem,
					  NIL);
	ReleaseBuffer(buf);		/* zsbt_recompress_replace released */

	pfree(deleteditem);
}

TM_Result
zsbt_lock_item(Relation rel, AttrNumber attno, zstid tid,
			   TransactionId xid, CommandId cid, Snapshot snapshot,
			   LockTupleMode lockmode, LockWaitPolicy wait_policy,
			   TM_FailureData *hufd)
{
	Buffer		buf;
	ZSSingleBtreeItem *item;
	TM_Result	result;
	bool		keep_old_undo_ptr = true;
	ZSUndoRecPtr undorecptr;
	ZSSingleBtreeItem *newitem;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_fetch(rel, attno, snapshot, tid, &buf);
	if (item == NULL)
	{
		/*
		 * or should this be TM_Invisible? The heapam at least just throws
		 * an error, I think..
		 */
		elog(ERROR, "could not find tuple to delete with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
	}
	result = zs_SatisfiesUpdate(rel, snapshot, (ZSBtreeItem *) item, &keep_old_undo_ptr, hufd);
	if (result != TM_Ok)
	{
		UnlockReleaseBuffer(buf);
		/* FIXME: We should fill TM_FailureData *hufd correctly */
		return result;
	}

	if ((item->t_flags & ZSBT_DELETED) != 0)
		elog(ERROR, "cannot lock deleted tuple");

	if ((item->t_flags & ZSBT_UPDATED) != 0)
		elog(ERROR, "cannot lock updated tuple");

	/* Create UNDO record. */
	{
		ZSUndoRec_TupleLock undorec;

		undorec.rec.size = sizeof(ZSUndoRec_TupleLock);
		undorec.rec.type = ZSUNDO_TYPE_TUPLE_LOCK;
		undorec.rec.attno = attno;
		undorec.rec.xid = xid;
		undorec.rec.cid = cid;
		undorec.rec.tid = tid;
		undorec.lockmode = lockmode;
		if (keep_old_undo_ptr)
			undorec.prevundorec = item->t_undo_ptr;
		else
			ZSUndoRecPtrInitialize(&undorec.prevundorec);

		undorecptr = zsundo_insert(rel, &undorec.rec);
	}

	/* Replace the item with an identical one, but with updated undo pointer. */
	newitem = palloc(item->t_size);
	memcpy(newitem, item, item->t_size);
	newitem->t_undo_ptr = undorecptr;

	zsbt_replace_item(rel, attno, buf,
					  item->t_tid, (ZSBtreeItem *) newitem,
					  NIL);
	ReleaseBuffer(buf);		/* zsbt_replace_item unlocked */

	pfree(newitem);

	return TM_Ok;
}

/*
 * Mark item with given TID as dead.
 *
 * This is used during VACUUM.
 */
void
zsbt_mark_item_dead(Relation rel, AttrNumber attno, zstid tid, ZSUndoRecPtr undoptr)
{
	Buffer		buf;
	ZSSingleBtreeItem *item;
	ZSSingleBtreeItem deaditem;

	/* Find the item to delete. (It could be compressed) */
	item = zsbt_fetch(rel, attno, NULL, tid, &buf);
	if (item == NULL)
	{
		elog(WARNING, "could not find tuple to remove with TID (%u, %u) for attribute %d",
			 ZSTidGetBlockNumber(tid), ZSTidGetOffsetNumber(tid), attno);
		return;
	}

	/* Replace the ZSBreeItem with a DEAD item. (Unless it's already dead) */
	if ((item->t_flags & ZSBT_DEAD) != 0)
	{
		UnlockReleaseBuffer(buf);
		return;
	}

	memset(&deaditem, 0, offsetof(ZSSingleBtreeItem, t_payload));
	deaditem.t_tid = tid;
	deaditem.t_size = sizeof(ZSSingleBtreeItem);
	deaditem.t_flags = ZSBT_DEAD;
	deaditem.t_undo_ptr = undoptr;

	zsbt_replace_item(rel, attno, buf,
					  tid, (ZSBtreeItem *) &deaditem,
					  NIL);
	ReleaseBuffer(buf); 	/* zsbt_replace_item released */
}

/* ----------------------------------------------------------------
 *						 Internal routines
 * ----------------------------------------------------------------
 */

/*
 * Find the leaf page containing the given key TID.
 */
static Buffer
zsbt_descend(Relation rel, BlockNumber rootblk, zstid key)
{
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);		/* TODO: shared */
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (opaque->zs_level != nextlevel)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level == 0)
			return buf;

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (key >= opaque->zs_hikey)
		{
			/* follow the right-link */
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			/* follow the downlink */
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ZSTidGetBlockNumber(key), ZSTidGetOffsetNumber(key));
			next = items[itemno].childblk;
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Re-find the parent page containing downlink for given block.
 * The returned page is exclusive-locked, and *itemno_p is set to the
 * position of the downlink in the parent.
 *
 * If 'childblk' is the root, returns InvalidBuffer.
 */
static Buffer
zsbt_find_downlink(Relation rel, AttrNumber attno,
				   zstid key, BlockNumber childblk, int level,
				   int *itemno_p)
{
	BlockNumber rootblk;
	int16		attlen;
	bool		attbyval;
	BlockNumber next;
	Buffer		buf;
	Page		page;
	ZSBtreePageOpaque *opaque;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	int			nextlevel = -1;

	/* start from root */
	rootblk = zsmeta_get_root_for_attribute(rel, attno, true, &attlen, &attbyval);
	if (rootblk == childblk)
		return InvalidBuffer;

	/* XXX: this is mostly the same as zsbt_descend, but we stop at an internal
	 * page instead of descending all the way down to leaf */
	next = rootblk;
	for (;;)
	{
		buf = ReadBuffer(rel, next);
		LockBuffer(buf, BUFFER_LOCK_EXCLUSIVE);
		page = BufferGetPage(buf);
		opaque = ZSBtreePageGetOpaque(page);

		if (nextlevel == -1)
			nextlevel = opaque->zs_level;
		else if (nextlevel != opaque->zs_level)
			elog(ERROR, "unexpected level encountered when descending tree");

		if (opaque->zs_level <= level)
			elog(ERROR, "unexpected page level encountered");

		/*
		 * Do we need to walk right? This could happen if the page was concurrently split.
		 */
		if (key >= opaque->zs_hikey)
		{
			next = opaque->zs_next;
			if (next == InvalidBlockNumber)
				elog(ERROR, "fell off the end of btree");
		}
		else
		{
			items = ZSBtreeInternalPageGetItems(page);
			nitems = ZSBtreeInternalPageGetNumItems(page);

			itemno = zsbt_binsrch_internal(key, items, nitems);
			if (itemno < 0)
				elog(ERROR, "could not descend tree for tid (%u, %u)",
					 ZSTidGetBlockNumber(key), ZSTidGetOffsetNumber(key));

			if (opaque->zs_level == level + 1)
			{
				if (items[itemno].childblk != childblk)
					elog(ERROR, "could not re-find downlink for block %u", childblk);
				*itemno_p = itemno;
				return buf;
			}

			next = items[itemno].childblk;
			nextlevel--;
		}
		UnlockReleaseBuffer(buf);
	}
}

/*
 * Create a new btree root page, containing two downlinks.
 *
 * NOTE: the very first root page of a btree, which is also the leaf, is created
 * in zsmeta_get_root_for_attribute(), not here.
 */
static void
zsbt_newroot(Relation rel, AttrNumber attno, int level,
			 zstid key1, BlockNumber blk1,
			 zstid key2, BlockNumber blk2,
			 Buffer leftchildbuf)
{
	ZSBtreePageOpaque *opaque;
	ZSBtreePageOpaque *leftchildopaque;
	Buffer		buf;
	Page		page;
	ZSBtreeInternalPageItem *items;
	Buffer		metabuf;

	metabuf = ReadBuffer(rel, ZS_META_BLK);
	LockBuffer(metabuf, BUFFER_LOCK_EXCLUSIVE);

	Assert(key1 < key2);

	buf = zs_getnewbuf(rel);
	page = BufferGetPage(buf);
	PageInit(page, BLCKSZ, sizeof(ZSBtreePageOpaque));
	opaque = ZSBtreePageGetOpaque(page);
	opaque->zs_attno = attno;
	opaque->zs_next = InvalidBlockNumber;
	opaque->zs_lokey = MinZSTid;
	opaque->zs_hikey = MaxPlusOneZSTid;
	opaque->zs_level = level;
	opaque->zs_flags = 0;
	opaque->zs_page_id = ZS_BTREE_PAGE_ID;

	items = ZSBtreeInternalPageGetItems(page);
	items[0].tid = key1;
	items[0].childblk =  blk1;
	items[1].tid = key2;
	items[1].childblk = blk2;
	((PageHeader) page)->pd_lower += 2 * sizeof(ZSBtreeInternalPageItem);
	Assert(ZSBtreeInternalPageGetNumItems(page) == 2);

	/* clear the follow-right flag on left child */
	leftchildopaque = ZSBtreePageGetOpaque(BufferGetPage(leftchildbuf));
	leftchildopaque->zs_flags &= ~ZS_FOLLOW_RIGHT;

	/* TODO: wal-log all, including metapage */

	MarkBufferDirty(buf);
	MarkBufferDirty(leftchildbuf);

	/* Before exiting, update the metapage */
	zsmeta_update_root_for_attribute(rel, attno, metabuf, BufferGetBlockNumber(buf));

	UnlockReleaseBuffer(leftchildbuf);
	UnlockReleaseBuffer(buf);
	UnlockReleaseBuffer(metabuf);
}

/*
 * After page split, insert the downlink of 'rightblkno' to the parent.
 *
 * On entry, 'leftbuf' must be pinned exclusive-locked. It is released on exit.
 */
static void
zsbt_insert_downlink(Relation rel, AttrNumber attno, Buffer leftbuf,
					 zstid rightlokey, BlockNumber rightblkno)
{
	BlockNumber	leftblkno = BufferGetBlockNumber(leftbuf);
	Page		leftpage = BufferGetPage(leftbuf);
	ZSBtreePageOpaque *leftopaque = ZSBtreePageGetOpaque(leftpage);
	zstid		leftlokey = leftopaque->zs_lokey;
	ZSBtreeInternalPageItem *items;
	int			nitems;
	int			itemno;
	Buffer		parentbuf;
	Page		parentpage;

	/*
	 * re-find parent
	 *
	 * TODO: this is a bit inefficient. Usually, we have just descended the
	 * tree, and if we just remembered the path we descended, we could just
	 * walk back up.
	 */
	parentbuf = zsbt_find_downlink(rel, attno, leftlokey, leftblkno, leftopaque->zs_level, &itemno);
	if (parentbuf == InvalidBuffer)
	{
		zsbt_newroot(rel, attno, leftopaque->zs_level + 1,
					 leftlokey, BufferGetBlockNumber(leftbuf),
					 rightlokey, rightblkno, leftbuf);
		return;
	}
	parentpage = BufferGetPage(parentbuf);

	/* Find the position in the parent for the downlink */
	items = ZSBtreeInternalPageGetItems(parentpage);
	nitems = ZSBtreeInternalPageGetNumItems(parentpage);
	itemno = zsbt_binsrch_internal(rightlokey, items, nitems);

	/* sanity checks */
	if (itemno < 0 || items[itemno].tid != leftlokey ||
		items[itemno].childblk != leftblkno)
	{
		elog(ERROR, "could not find downlink for block %u TID (%u, %u)",
			 leftblkno, ZSTidGetBlockNumber(leftlokey),
			 ZSTidGetOffsetNumber(leftlokey));
	}
	itemno++;

	if (ZSBtreeInternalPageIsFull(parentpage))
	{
		/* split internal page */
		zsbt_split_internal_page(rel, attno, parentbuf, leftbuf, itemno, rightlokey, rightblkno);
	}
	else
	{
		/* insert the new downlink for the right page. */
		memmove(&items[itemno + 1],
				&items[itemno],
				(nitems - itemno) * sizeof(ZSBtreeInternalPageItem));
		items[itemno].tid = rightlokey;
		items[itemno].childblk = rightblkno;
		((PageHeader) parentpage)->pd_lower += sizeof(ZSBtreeInternalPageItem);

		leftopaque->zs_flags &= ~ZS_FOLLOW_RIGHT;

		/* TODO: WAL-log */

		MarkBufferDirty(leftbuf);
		MarkBufferDirty(parentbuf);
		UnlockReleaseBuffer(leftbuf);
		UnlockReleaseBuffer(parentbuf);
	}
}

/*
 * Split an internal page.
 *
 * The new downlink specified by 'newkey' and 'childblk' is inserted to
 * position 'newoff', on 'leftbuf'. The page is split.
 */
static void
zsbt_split_internal_page(Relation rel, AttrNumber attno, Buffer leftbuf, Buffer childbuf,
						 OffsetNumber newoff, zstid newkey, BlockNumber childblk)
{
	Buffer		rightbuf;
	Page		origpage = BufferGetPage(leftbuf);
	Page		leftpage;
	Page		rightpage;
	BlockNumber rightblkno;
	ZSBtreePageOpaque *leftopaque;
	ZSBtreePageOpaque *rightopaque;
	ZSBtreeInternalPageItem *origitems;
	ZSBtreeInternalPageItem *leftitems;
	ZSBtreeInternalPageItem *rightitems;
	int			orignitems;
	int			leftnitems;
	int			rightnitems;
	int			splitpoint;
	zstid		splittid;
	bool		newitemonleft;
	int			i;
	ZSBtreeInternalPageItem newitem;

	leftpage = PageGetTempPageCopySpecial(origpage);
	leftopaque = ZSBtreePageGetOpaque(leftpage);
	Assert(leftopaque->zs_level > 0);
	/* any previous incomplete split must be finished first */
	Assert((leftopaque->zs_flags & ZS_FOLLOW_RIGHT) == 0);

	rightbuf = zs_getnewbuf(rel);
	rightpage = BufferGetPage(rightbuf);
	rightblkno = BufferGetBlockNumber(rightbuf);
	PageInit(rightpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	rightopaque = ZSBtreePageGetOpaque(rightpage);

	/*
	 * Figure out the split point.
	 *
	 * TODO: currently, always do 90/10 split.
	 */
	origitems = ZSBtreeInternalPageGetItems(origpage);
	orignitems = ZSBtreeInternalPageGetNumItems(origpage);
	splitpoint = orignitems * 0.9;
	splittid = origitems[splitpoint].tid;
	newitemonleft = (newkey < splittid);

	/* Set up the page headers */
	rightopaque->zs_attno = attno;
	rightopaque->zs_next = leftopaque->zs_next;
	rightopaque->zs_lokey = splittid;
	rightopaque->zs_hikey = leftopaque->zs_hikey;
	rightopaque->zs_level = leftopaque->zs_level;
	rightopaque->zs_flags = 0;
	rightopaque->zs_page_id = ZS_BTREE_PAGE_ID;

	leftopaque->zs_next = rightblkno;
	leftopaque->zs_hikey = splittid;
	leftopaque->zs_flags |= ZS_FOLLOW_RIGHT;

	/* copy the items */
	leftitems = ZSBtreeInternalPageGetItems(leftpage);
	leftnitems = 0;
	rightitems = ZSBtreeInternalPageGetItems(rightpage);
	rightnitems = 0;

	newitem.tid = newkey;
	newitem.childblk = childblk;

	for (i = 0; i < orignitems; i++)
	{
		if (i == newoff)
		{
			if (newitemonleft)
				leftitems[leftnitems++] = newitem;
			else
				rightitems[rightnitems++] = newitem;
		}

		if (i < splitpoint)
			leftitems[leftnitems++] = origitems[i];
		else
			rightitems[rightnitems++] = origitems[i];
	}
	/* cope with possibility that newitem goes at the end */
	if (i <= newoff)
	{
		Assert(!newitemonleft);
		rightitems[rightnitems++] = newitem;
	}
	((PageHeader) leftpage)->pd_lower += leftnitems * sizeof(ZSBtreeInternalPageItem);
	((PageHeader) rightpage)->pd_lower += rightnitems * sizeof(ZSBtreeInternalPageItem);

	Assert(leftnitems + rightnitems == orignitems + 1);

	PageRestoreTempPage(leftpage, origpage);

	/* TODO: WAL-logging */
	MarkBufferDirty(leftbuf);
	MarkBufferDirty(rightbuf);

	MarkBufferDirty(childbuf);
	ZSBtreePageGetOpaque(BufferGetPage(childbuf))->zs_flags &= ~ZS_FOLLOW_RIGHT;
	UnlockReleaseBuffer(childbuf);

	UnlockReleaseBuffer(rightbuf);

	/* recurse to insert downlink. (this releases 'leftbuf') */
	zsbt_insert_downlink(rel, attno, leftbuf, splittid, rightblkno);
}

static ZSSingleBtreeItem *
zsbt_fetch(Relation rel, AttrNumber attno, Snapshot snapshot, zstid tid,
		   Buffer *buf_p)
{
	BlockNumber	rootblk;
	int16		attlen;
	bool		attbyval;
	Buffer		buf;
	Page		page;
	ZSBtreeItem *item = NULL;
	bool		found = false;
	OffsetNumber maxoff;
	OffsetNumber off;

	rootblk = zsmeta_get_root_for_attribute(rel, attno, false, &attlen, &attbyval);

	if (rootblk == InvalidBlockNumber)
	{
		*buf_p = InvalidBuffer;
		return NULL;
	}

	buf = zsbt_descend(rel, rootblk, tid);
	page = BufferGetPage(buf);

	/* Find the item on the page that covers the target TID */
	maxoff = PageGetMaxOffsetNumber(page);
	for (off = FirstOffsetNumber; off <= maxoff; off++)
	{
		ItemId		iid = PageGetItemId(page, off);
		item = (ZSBtreeItem *) PageGetItem(page, iid);

		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			ZSCompressedBtreeItem *citem = (ZSCompressedBtreeItem *) item;
			ZSDecompressContext decompressor;

			zs_decompress_init(&decompressor);
			zs_decompress_chunk(&decompressor, citem);

			while ((item = zs_decompress_read_item(&decompressor)) != NULL)
			{
				zstid		lasttid = zsbt_item_lasttid(item);

				if (item->t_tid <= tid && lasttid >= tid)
				{
					found = true;
					break;
				}
			}
			if (found)
			{
				/* FIXME: decompressor is leaked. Can't free it yet, because we still
				 * need to access the item below
				 */
				break;
			}
			zs_decompress_free(&decompressor);
		}
		else
		{
			zstid		lasttid = zsbt_item_lasttid(item);

			if (item->t_tid <= tid && lasttid >= tid)
			{
				found = true;
				break;
			}
		}
	}

	if (found && snapshot)
	{
		/*
		 * Ok, we have the item that covers the target TID now, in 'item'. Check
		 * if it's visible.
		 */
		/* FIXME: dummmy scan */
		ZSBtreeScan scan;
		memset(&scan, 0, sizeof(scan));
		scan.rel = rel;
		scan.snapshot = snapshot;

		if (!zs_SatisfiesVisibility(&scan, item))
			found = false;
	}

	if (found)
	{
		ZSSingleBtreeItem *result;

		if ((item->t_flags & ZSBT_ARRAY) != 0)
		{
			ZSArrayBtreeItem *aitem = (ZSArrayBtreeItem *) item;
			int			elemno = tid - aitem->t_tid;
			char	   *dataptr = NULL;
			int			datasz;
			int			resultsize;

			Assert(elemno < aitem->t_nelements);

			if ((item->t_flags & ZSBT_NULL) == 0)
			{
				if (attlen > 0)
				{
					dataptr = aitem->t_payload + elemno * attlen;
					datasz = attlen;
				}
				else
				{
					dataptr = aitem->t_payload;
					for (int i = 0; i < elemno; i++)
					{
						dataptr += zs_datumGetSize(PointerGetDatum(dataptr), attbyval, attlen);
					}
					datasz = zs_datumGetSize(PointerGetDatum(dataptr), attbyval, attlen);
				}
			}
			else
				datasz = 0;

			resultsize = offsetof(ZSSingleBtreeItem, t_payload) + datasz;
			result = palloc(resultsize);
			memset(result, 0, offsetof(ZSSingleBtreeItem, t_payload)); /* zero padding */
			result->t_tid = tid;
			result->t_flags = item->t_flags & ~ZSBT_ARRAY;
			result->t_size = resultsize;
			result->t_undo_ptr = aitem->t_undo_ptr;
			if (datasz > 0)
				memcpy(result->t_payload, dataptr, datasz);
		}
		else
		{
			/* single item */
			result = (ZSSingleBtreeItem *) item;
		}

		*buf_p = buf;
		return result;
	}
	else
	{
		UnlockReleaseBuffer(buf);
		*buf_p = InvalidBuffer;
		return NULL;
	}
}

/*
 * This helper function is used to implement INSERT, UPDATE and DELETE.
 *
 * If 'olditem' is not NULL, then 'olditem' on the page is replaced with
 * 'replacementitem'. 'replacementitem' can be NULL, to remove an old item.
 *
 * If 'newitems' is not empty, the items in the list are added to the page,
 * to the correct position. FIXME: Actually, they're always just added to
 * the end of the page, and that better be the correct position.
 *
 * This function handles decompressing and recompressing items, and splitting
 * the page if needed.
 */
static void
zsbt_replace_item(Relation rel, AttrNumber attno, Buffer buf,
				  zstid oldtid,
				  ZSBtreeItem *replacementitem,
				  List       *newitems)
{
	Form_pg_attribute attr = &rel->rd_att->attrs[attno - 1];
	int16		attlen = attr->attlen;
	bool		attbyval = attr->attbyval;
	Page		page = BufferGetPage(buf);
	OffsetNumber off;
	OffsetNumber maxoff;
	List	   *items;
	bool		found_old_item = false;
	/* We might need to decompress up to two previously compressed items */
	ZSDecompressContext decompressor;
	bool		decompressor_used = false;
	bool		decompressing;

	if (replacementitem)
		Assert(replacementitem->t_tid == oldtid);

	/*
	 * TODO: It would be good to have a fast path, for the common case that we're
	 * just adding items to the end.
	 */

	/* Loop through all old items on the page */
	items = NIL;
	maxoff = PageGetMaxOffsetNumber(page);
	decompressing = false;
	off = 1;
	for (;;)
	{
		ZSBtreeItem *item;

		/*
		 * Get the next item to process. If we're decompressing, get the next
		 * tuple from the decompressor, otherwise get the next item from the page.
		 */
		if (decompressing)
		{
			item = zs_decompress_read_item(&decompressor);
			if (!item)
			{
				decompressing = false;
				continue;
			}
		}
		else if (off <= maxoff)
		{
			ItemId		iid = PageGetItemId(page, off);

			item = (ZSBtreeItem *) PageGetItem(page, iid);
			off++;

		}
		else
		{
			/* out of items */
			break;
		}

		/* we now have an item to process, either straight from the page or from
		 * the decompressor */
		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			zstid		item_lasttid = zsbt_item_lasttid(item);

			/* there shouldn't nested compressed items */
			if (decompressing)
				elog(ERROR, "nested compressed items on zedstore page not supported");

			if (oldtid != InvalidZSTid && item->t_tid <= oldtid && oldtid <= item_lasttid)
			{
				ZSCompressedBtreeItem *citem = (ZSCompressedBtreeItem *) item;

				/* Found it, this compressed item covers the target or the new TID. */
				/* We have to decompress it, and recompress */
				Assert(!decompressor_used);

				zs_decompress_init(&decompressor);
				zs_decompress_chunk(&decompressor, citem);
				decompressor_used = true;
				decompressing = true;
				continue;
			}
			else
			{
				/* keep this compressed item as it is */
				items = lappend(items, item);
			}
		}
		else if ((item->t_flags & ZSBT_ARRAY) != 0)
		{
			/* array item */
			ZSArrayBtreeItem *aitem = (ZSArrayBtreeItem *) item;
			zstid		item_lasttid = zsbt_item_lasttid(item);

			if (oldtid != InvalidZSTid && item->t_tid <= oldtid && oldtid <= item_lasttid)
			{
				/*
				 * The target TID is currently part of an array item. We have to split
				 * the array item into two, and put the replacement item in the middle.
				 */
				int			cutoff;
				Size		olddatalen;
				int			nelements = aitem->t_nelements;
				bool		isnull = (aitem->t_flags & ZSBT_NULL) != 0;
				char	   *dataptr;

				cutoff = oldtid - item->t_tid;

				/* Array slice before the target TID */
				dataptr = aitem->t_payload;
				if (cutoff > 0)
				{
					ZSBtreeItem *item1;
					Size		datalen1;

					datalen1 = zsbt_get_array_slice_len(attlen, attbyval, isnull,
														dataptr, cutoff);
					item1 = zsbt_create_item(attlen, attbyval, aitem->t_tid, aitem->t_undo_ptr,
											 cutoff, NULL, dataptr, datalen1, isnull);
					dataptr += datalen1;
					items = lappend(items, item1);
				}

				/*
				 * Skip over the target element, and store the replacement
				 * item, if any, in its place
				 */
				olddatalen = zsbt_get_array_slice_len(attlen, attbyval, isnull,
													  dataptr, 1);
				dataptr += olddatalen;
				if (replacementitem)
					items = lappend(items, replacementitem);

				/* Array slice after the target */
				if (cutoff + 1 < nelements)
				{
					ZSBtreeItem *item2;
					Size		datalen2;

					datalen2 = zsbt_get_array_slice_len(attlen, attbyval, isnull,
														dataptr, nelements - (cutoff + 1));
					item2 = zsbt_create_item(attlen, attbyval, oldtid + 1, aitem->t_undo_ptr,
											 nelements - (cutoff + 1), NULL, dataptr, datalen2, isnull);
					items = lappend(items, item2);
				}

				found_old_item = true;
			}
			else
				items = lappend(items, item);
		}
		else
		{
			/* single item */
			if (oldtid != InvalidZSTid && item->t_tid == oldtid)
			{
				Assert(!found_old_item);
				found_old_item = true;
				if (replacementitem)
					items = lappend(items, replacementitem);
			}
			else
				items = lappend(items, item);
		}
	}

	if (oldtid != InvalidZSTid && !found_old_item)
		elog(ERROR, "could not find old item to replace");

	/* Add any new items to the end */
	if (newitems)
		items = list_concat(items, newitems);

	/* Now pass the list to the recompressor. */
	IncrBufferRefCount(buf);
	zsbt_recompress_replace(rel, attno, buf, items);

	/*
	 * We can now free the decompression contexts. The pointers in the 'items' list
	 * point to decompression buffers, so we cannot free them until after writing out
	 * the pages.
	 */
	if (decompressor_used)
		zs_decompress_free(&decompressor);
	list_free(items);
}

/*
 * Recompressor routines
 */
typedef struct
{
	Page		currpage;
	ZSCompressContext compressor;
	int			compressed_items;
	List	   *pages;		/* first page writes over the old buffer,
							 * subsequent pages get newly-allocated buffers */

	int			total_items;
	int			total_compressed_items;
	int			total_already_compressed_items;

	AttrNumber	attno;
	zstid		hikey;
} zsbt_recompress_context;

static void
zsbt_recompress_newpage(zsbt_recompress_context *cxt, zstid nexttid)
{
	Page		newpage;
	ZSBtreePageOpaque *newopaque;

	if (cxt->currpage)
	{
		/* set the last tid on previous page */
		ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(cxt->currpage);

		oldopaque->zs_hikey = nexttid;
	}

	newpage = (Page) palloc(BLCKSZ);
	PageInit(newpage, BLCKSZ, sizeof(ZSBtreePageOpaque));
	cxt->pages = lappend(cxt->pages, newpage);
	cxt->currpage = newpage;

	newopaque = ZSBtreePageGetOpaque(newpage);
	newopaque->zs_attno = cxt->attno;
	newopaque->zs_next = InvalidBlockNumber; /* filled in later */
	newopaque->zs_lokey = nexttid;
	newopaque->zs_hikey = cxt->hikey;		/* overwritten later, if this is not last page */
	newopaque->zs_level = 0;
	newopaque->zs_flags = 0;
	newopaque->zs_page_id = ZS_BTREE_PAGE_ID;
}

static void
zsbt_recompress_add_to_page(zsbt_recompress_context *cxt, ZSBtreeItem *item)
{
	if (PageGetFreeSpace(cxt->currpage) < MAXALIGN(item->t_size))
		zsbt_recompress_newpage(cxt, item->t_tid);

	if (PageAddItemExtended(cxt->currpage,
							(Item) item, item->t_size,
							PageGetMaxOffsetNumber(cxt->currpage) + 1,
							PAI_OVERWRITE) == InvalidOffsetNumber)
		elog(ERROR, "could not add item to page while recompressing");

	cxt->total_items++;
}

static bool
zsbt_recompress_add_to_compressor(zsbt_recompress_context *cxt, ZSBtreeItem *item)
{
	bool		result;

	if (cxt->compressed_items == 0)
		zs_compress_begin(&cxt->compressor, PageGetFreeSpace(cxt->currpage));

	result = zs_compress_add(&cxt->compressor, item);
	if (result)
	{
		cxt->compressed_items++;

		cxt->total_compressed_items++;
	}

	return result;
}

static void
zsbt_recompress_flush(zsbt_recompress_context *cxt)
{
	ZSCompressedBtreeItem *citem;

	if (cxt->compressed_items == 0)
		return;

	citem = zs_compress_finish(&cxt->compressor);

	zsbt_recompress_add_to_page(cxt, (ZSBtreeItem *) citem);
	cxt->compressed_items = 0;
}

/*
 * Rewrite a leaf page, with given 'items' as the new content.
 *
 * If there are any uncompressed items in the list, we try to compress them.
 * Any already-compressed items are added as is.
 *
 * If the items no longer fit on the page, then the page is split. It is
 * entirely possible that they don't fit even on two pages; we split the page
 * into as many pages as needed. Hopefully not more than a few pages, though,
 * because otherwise you might hit limits on the number of buffer pins (with
 * tiny shared_buffers).
 *
 * On entry, 'oldbuf' must be pinned and exclusive-locked. On exit, the lock
 * is released, but it's still pinned.
 *
 * TODO: Try to combine single items, and existing array-items, into new array
 * items.
 */
static void
zsbt_recompress_replace(Relation rel, AttrNumber attno, Buffer oldbuf, List *items)
{
	ListCell   *lc;
	ListCell   *lc2;
	zsbt_recompress_context cxt;
	ZSBtreePageOpaque *oldopaque = ZSBtreePageGetOpaque(BufferGetPage(oldbuf));
	ZSUndoRecPtr recent_oldest_undo = { 0 };
	List	   *bufs;
	int			i;
	BlockNumber orignextblk;

	cxt.currpage = NULL;
	zs_compress_init(&cxt.compressor);
	cxt.compressed_items = 0;
	cxt.pages = NIL;
	cxt.attno = attno;
	cxt.hikey = oldopaque->zs_hikey;

	cxt.total_items = 0;
	cxt.total_compressed_items = 0;
	cxt.total_already_compressed_items = 0;

	zsbt_recompress_newpage(&cxt, oldopaque->zs_lokey);

	foreach(lc, items)
	{
		ZSBtreeItem *item = (ZSBtreeItem *) lfirst(lc);

		/* We can leave out any old-enough DEAD items */
		if ((item->t_flags & ZSBT_DEAD) != 0)
		{
			ZSBtreeItem *uitem = (ZSBtreeItem *) item;

			if (recent_oldest_undo.counter == 0)
				recent_oldest_undo = zsundo_get_oldest_undo_ptr(rel);

			if (zsbt_item_undoptr(uitem).counter < recent_oldest_undo.counter)
				continue;
		}

		if ((item->t_flags & ZSBT_COMPRESSED) != 0)
		{
			/* already compressed, add as it is. */
			zsbt_recompress_flush(&cxt);
			cxt.total_already_compressed_items++;
			zsbt_recompress_add_to_page(&cxt, item);
		}
		else
		{
			/* try to add this item to the compressor */
			if (!zsbt_recompress_add_to_compressor(&cxt, item))
			{
				if (cxt.compressed_items > 0)
				{
					/* flush, and retry */
					zsbt_recompress_flush(&cxt);

					if (!zsbt_recompress_add_to_compressor(&cxt, item))
					{
						/* could not compress, even on its own. Store it uncompressed, then */
						zsbt_recompress_add_to_page(&cxt, item);
					}
				}
				else
				{
					/* could not compress, even on its own. Store it uncompressed, then */
					zsbt_recompress_add_to_page(&cxt, item);
				}
			}
		}
	}

	/* flush the last one, if any */
	zsbt_recompress_flush(&cxt);

	zs_compress_free(&cxt.compressor);

	/*
	 * Ok, we now have a list of pages, to replace the original page, as private
	 * in-memory copies. Allocate buffers for them, and write them out
	 *
	 * allocate all the pages before entering critical section, so that
	 * out-of-disk-space doesn't lead to PANIC
	 */
	bufs = list_make1_int(oldbuf);
	for (i = 0; i < list_length(cxt.pages) - 1; i++)
	{
		Buffer		newbuf = zs_getnewbuf(rel);

		bufs = lappend_int(bufs, newbuf);
	}

	START_CRIT_SECTION();

	orignextblk = oldopaque->zs_next;
	forboth(lc, cxt.pages, lc2, bufs)
	{
		Page		page_copy = (Page) lfirst(lc);
		Buffer		buf = (Buffer) lfirst_int(lc2);
		Page		page = BufferGetPage(buf);
		ZSBtreePageOpaque *opaque;

		PageRestoreTempPage(page_copy, page);
		opaque = ZSBtreePageGetOpaque(page);

		/* TODO: WAL-log */
		if (lnext(lc2))
		{
			Buffer		nextbuf = (Buffer) lfirst_int(lnext(lc2));

			opaque->zs_next = BufferGetBlockNumber(nextbuf);
			opaque->zs_flags |= ZS_FOLLOW_RIGHT;
		}
		else
		{
			/* last one in the chain. */
			opaque->zs_next = orignextblk;
		}

		MarkBufferDirty(buf);
	}
	list_free(cxt.pages);

	END_CRIT_SECTION();

	/* If we had to split, insert downlinks for the new pages. */
	while (list_length(bufs) > 1)
	{
		Buffer		leftbuf = (Buffer) linitial_int(bufs);
		Buffer		rightbuf = (Buffer) lsecond_int(bufs);

		zsbt_insert_downlink(rel, attno, leftbuf,
							 ZSBtreePageGetOpaque(BufferGetPage(leftbuf))->zs_hikey,
							 BufferGetBlockNumber(rightbuf));
		/* zsbt_insert_downlink() released leftbuf */
		bufs = list_delete_first(bufs);
	}
	/* release the last page */
	UnlockReleaseBuffer((Buffer) linitial_int(bufs));
	list_free(bufs);
}

static int
zsbt_binsrch_internal(zstid key, ZSBtreeInternalPageItem *arr, int arr_elems)
{
	int			low,
		high,
		mid;

	low = 0;
	high = arr_elems;
	while (high > low)
	{
		mid = low + (high - low) / 2;

		if (key >= arr[mid].tid)
			low = mid + 1;
		else
			high = mid;
	}
	return low - 1;
}