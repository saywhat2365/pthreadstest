/*
  +----------------------------------------------------------------------+
  | pthreads                                                             |
  +----------------------------------------------------------------------+
  | Copyright (c) Joe Watkins 2012 - 2014                                |
  +----------------------------------------------------------------------+
  | This source file is subject to version 3.01 of the PHP license,      |
  | that is bundled with this package in the file LICENSE, and is        |
  | available through the world-wide-web at the following url:           |
  | http://www.php.net/license/3_01.txt                                  |
  | If you did not receive a copy of the PHP license and are unable to   |
  | obtain it through the world-wide-web, please send a note to          |
  | license@php.net so we can mail you a copy immediately.               |
  +----------------------------------------------------------------------+
  | Author: Joe Watkins <joe.watkins@live.co.uk>                         |
  +----------------------------------------------------------------------+
 */
#ifndef HAVE_PTHREADS_STORE
#define HAVE_PTHREADS_STORE

#ifndef HAVE_PTHREADS_STORE_H
#	include <src/store.h>
#endif

#ifndef HAVE_PTHREADS_THREAD_H
#	include <src/thread.h>
#endif

#ifndef HAVE_PTHREADS_OBJECT_H
#	include <src/object.h>
#endif

#ifndef HAVE_PTHREADS_RESOURCES_H
#	include <src/resources.h>
#endif

#ifndef HAVE_PTHREADS_COPY_H
#	include <src/copy.h>
#endif

typedef struct _pthreads_storage {
	zend_uchar 	type;
	size_t 		length;
	zend_bool 	exists;
	union {
	    long    lval;
	    double  dval;
	} simple;
	void    *data;
} pthreads_storage;

#ifndef Z_OBJ_P
#	define Z_OBJ_P(zval_p) ((zend_object*)(EG(objects_store).object_buckets[Z_OBJ_HANDLE_P(zval_p)].bucket.obj.object))
#endif

/* {{{ statics */
static void pthreads_store_create(pthreads_storage *storage, zval *pzval, zend_bool complex TSRMLS_DC);
static int pthreads_store_convert(pthreads_storage *storage, zval *pzval TSRMLS_DC);
static int pthreads_store_tostring(zval *pzval, char **pstring, size_t *slength, zend_bool complex TSRMLS_DC);
static int pthreads_store_tozval(zval *pzval, char *pstring, size_t slength TSRMLS_DC);
static int pthreads_store_remove_complex_recursive(zval **pzval TSRMLS_DC);
static int pthreads_store_validate_object(zval **pzval TSRMLS_DC);
static int pthreads_store_remove_complex(zval **pzval TSRMLS_DC);
static void pthreads_store_storage_dtor (pthreads_storage *element);
/* }}} */

/* {{{ allocate storage for an object */
pthreads_store pthreads_store_alloc(TSRMLS_D) {
	pthreads_store store = malloc(sizeof(*store));
	
	if (store) {
		if (zend_hash_init(&store->table, 8, NULL, (dtor_func_t) pthreads_store_storage_dtor, 1)==SUCCESS){
			if ((store->lock = pthreads_lock_alloc(TSRMLS_C))) {
			    store->next = 0L;
			    
				return store;
			}
			zend_hash_destroy(&store->table);
		}
		free(store);
	}
	return NULL;
} /* }}} */

/* {{{ lock storage, userland only */
zend_bool pthreads_store_lock(zval *this_ptr TSRMLS_DC) {	
	PTHREAD pobject = PTHREADS_FETCH_FROM(getThis());
	if (pobject) {
		return pthreads_lock_acquire(
			pobject->store->lock,
			&pobject->hold TSRMLS_CC
		);
	} else return 0;
} /* }}} */

/* {{{ unlock storage, userland only */
zend_bool pthreads_store_unlock(zval *this_ptr TSRMLS_DC) {
	PTHREAD pobject = PTHREADS_FETCH_FROM(getThis());
	if (pobject) {
		return pthreads_lock_release(
			pobject->store->lock,
			pobject->hold TSRMLS_CC
		);
	} else return 0;
} /* }}} */

/* {{{ delete a value from the store */
int pthreads_store_delete(pthreads_store store, char *key, int keyl TSRMLS_DC) {
	int result = FAILURE;
	zend_bool locked;
	
	if (store) {
		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
			if (zend_hash_exists(&store->table, key, keyl+1)) {
				if (zend_hash_del(&store->table, key, keyl+1)!=SUCCESS) {
					result = FAILURE;
				} else result = SUCCESS;
			} else result = SUCCESS;
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		} else result = FAILURE;
	}
	return result;
}
/* }}} */

/* {{{ isset helper for handlers */
zend_bool pthreads_store_isset(pthreads_store store, char *key, int keyl, int has_set_exists TSRMLS_DC) {
	zend_bool locked = 0, isset = 0;
	if (store) {
		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
		    pthreads_storage *storage;
		    
		    isset = (zend_hash_find(&store->table, key, keyl+1, (void**)&storage) == SUCCESS);
		    
		    if (has_set_exists) {
	            switch (storage->type) {
	                case IS_LONG:
	                case IS_BOOL:
	                    if (storage->simple.lval == 0)
	                        isset = 0;
	                break;
	                
	                case IS_ARRAY:
	                    if (storage->length == 0)
	                        isset = 0;
	                break;
	                
	                case IS_STRING: switch (storage->length) {
	                    case 0:
	                        isset = 0;
	                    break;
	                    
	                    case 1:
	                        if (memcmp(storage->data, "0", 1) == SUCCESS)
	                            isset = 0;
	                    break;
	                } break;
	                
	                case IS_DOUBLE:
	                    if (storage->simple.dval == 0.0)
	                        isset = 0;
	                break;
	                    
	                case IS_NULL:
	                    isset = 0;
	                break;
	            }
	        } else if (isset) {
                switch (storage->type) {
                    case IS_NULL:
                        isset = 0;
                    break;
                }
            }
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		}
	}
	return isset;
} /* }}} */

/* {{{ read a value from store */
int pthreads_store_read(pthreads_store store, char *key, int keyl, zval **read TSRMLS_DC) {
	zend_bool locked = 0;
	int result = FAILURE;
	if (store) {
		MAKE_STD_ZVAL(*read);

		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
			pthreads_storage *storage = NULL;

			if (zend_hash_find(&store->table, key, keyl+1, (void**)&storage)==SUCCESS) {
				result = pthreads_store_convert(
				    storage, *read TSRMLS_CC);
			}
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		}

		if (result == SUCCESS) {
			Z_SET_REFCOUNT_PP(read, 0);
		} else {
			FREE_ZVAL(*read);
			*read = EG(
				uninitialized_zval_ptr
			);
			Z_ADDREF_P(*read);
		}
	}
	return SUCCESS;
} /* }}} */

/* {{{ write a value to store */
int pthreads_store_write(pthreads_store store, char *key, int keyl, zval **write TSRMLS_DC) {
	int result = FAILURE;
	zend_bool locked;
	
	if (store) {
		pthreads_storage storage;
		
		pthreads_store_create(
		    &storage, *write, 1 TSRMLS_CC);
		
		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
			if (zend_hash_update(
			    &store->table, key, keyl+1, (void**) &storage, sizeof(pthreads_storage), NULL)==SUCCESS) {
				result = SUCCESS;
			} else pthreads_store_storage_dtor(&storage);
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		} else pthreads_store_storage_dtor(&storage);
	}
	
	return result;
} /* }}} */

/* {{{ seperate a zval using internals */
int pthreads_store_separate(zval * pzval, zval **separated, zend_bool allocate, zend_bool complex TSRMLS_DC) {
	int result = FAILURE;
	pthreads_storage storage;
	
	if (allocate) {
		MAKE_STD_ZVAL(*separated);
	}
	
	if (pzval) {
        pthreads_store_create(&storage, pzval, complex TSRMLS_CC);
		 
		result = pthreads_store_convert(
		    &storage, *separated TSRMLS_CC);

		if (result == SUCCESS)
			pthreads_store_storage_dtor(&storage);
	    else Z_TYPE_PP(separated) = IS_NULL;
	} else Z_TYPE_PP(separated) = IS_NULL;
	
	return result;
} /* }}} */

/* {{{ seperate a zval using internals */
int pthreads_store_separate_from(zval * pzval, zval **separated, zend_bool allocate, zend_bool complex, void ***parent TSRMLS_DC) {
	int result = FAILURE;
	pthreads_storage storage;
	
	if (allocate) {
		MAKE_STD_ZVAL(*separated);
	}
	
	if (pzval) {
        pthreads_store_create(&storage, pzval, complex, parent);
		 
		result = pthreads_store_convert(
		    &storage, *separated TSRMLS_CC);

		if (result == SUCCESS)
			pthreads_store_storage_dtor(&storage);
	    else Z_TYPE_PP(separated) = IS_NULL;
	} else Z_TYPE_PP(separated) = IS_NULL;
	
	return result;
} /* }}} */

/* {{{ count properties */
int pthreads_store_count(zval *object, long *count TSRMLS_DC) {
   PTHREAD pthreads = PTHREADS_FETCH_FROM(object);
   
   if (pthreads) {
    if (pthreads_store_lock(object TSRMLS_CC)) {
        (*count) = zend_hash_num_elements(
            &pthreads->store->table);
        pthreads_store_unlock(object TSRMLS_CC);
    } else (*count) = 0L;
   } else (*count) = 0L;
   
   return SUCCESS;
} /* }}} */

/* {{{ shift member */
int pthreads_store_shift(zval *object, zval **member TSRMLS_DC) {
   PTHREAD pthreads = PTHREADS_FETCH_FROM(object);
   
   if (pthreads) {
    if (pthreads_store_lock(object TSRMLS_CC)) {
        pthreads_storage *storage;
        HashPosition position;
        HashTable *table = &pthreads->store->table;
        
        zend_hash_internal_pointer_reset_ex(table, &position);
        
        if (zend_hash_get_current_data_ex(table, (void**) &storage, &position) == SUCCESS) {
            char *key;
            uint klen;
            ulong idx;
            int flag;

            pthreads_store_convert(storage, *member TSRMLS_CC);

            flag = zend_hash_get_current_key_ex(table, &key, &klen, &idx, 0, &position) == HASH_KEY_IS_STRING ? HASH_DEL_KEY : HASH_DEL_INDEX;
            zend_hash_del_key_or_index(table, key, klen, idx, flag);
        } else ZVAL_NULL(*member);
        
        pthreads_store_unlock(object TSRMLS_CC);
        
        return SUCCESS;
    }
   }
   
   return FAILURE;
} /* }}} */

/* {{{ store chunk */
int pthreads_store_chunk(zval *object, long size, zend_bool preserve, zval **chunk TSRMLS_DC) {
   PTHREAD pthreads = PTHREADS_FETCH_FROM(object);
   
   if (pthreads) {
    if (pthreads_store_lock(object TSRMLS_CC)) {
        pthreads_storage *storage;
        HashPosition position;
        HashTable *table = &pthreads->store->table;
        
        zend_hash_internal_pointer_reset_ex(table, &position);
        
        array_init(*chunk);
        
        while ((zend_hash_num_elements(Z_ARRVAL_PP(chunk)) < size) &&
               (zend_hash_get_current_data_ex(table, (void**) &storage, &position) == SUCCESS)) {
            char *key;
            uint klen;
            ulong idx = 0L;
            zval *member;
            int ktype = 0;
            
            ALLOC_INIT_ZVAL(member);
            
            pthreads_store_convert(storage, member TSRMLS_CC);
           
            ktype = zend_hash_get_current_key_ex(table, &key, &klen, &idx, 0, &position);
            
            if (!preserve) {
                add_next_index_zval(*chunk, member);
            } else {
                if (ktype == HASH_KEY_IS_STRING) {
                    add_assoc_zval(*chunk, key, member);
                } else add_index_zval(*chunk, idx, member);
            }
            
            zend_hash_move_forward_ex(table, &position);
            
            zend_hash_del_key_or_index(table, 
                key, klen, idx, 
                (ktype == HASH_KEY_IS_STRING) ? HASH_DEL_KEY : HASH_DEL_INDEX);
        }
        
        pthreads_store_unlock(object TSRMLS_CC);
        
        return SUCCESS;
    }
   }
   
   return FAILURE;
} /* }}} */

/* {{{ pop member */
int pthreads_store_pop(zval *object, zval **member TSRMLS_DC) {
   PTHREAD pthreads = PTHREADS_FETCH_FROM(object);
   
   if (pthreads) {
    if (pthreads_store_lock(object TSRMLS_CC)) {
        pthreads_storage *storage;
        HashPosition position;
        HashTable *table = &pthreads->store->table;
        
        zend_hash_internal_pointer_end_ex(table, &position);
        
        if (zend_hash_get_current_data_ex(table, (void**) &storage, &position) == SUCCESS) {
            char *key;
            uint klen;
            ulong idx;
            int flag;
            
            pthreads_store_convert(storage, *member TSRMLS_CC);
            
            flag = zend_hash_get_current_key_ex(table, &key, &klen, &idx, 0, &position) == HASH_KEY_IS_STRING ? HASH_DEL_KEY : HASH_DEL_INDEX;
            zend_hash_del_key_or_index(table, key, klen, idx, flag);
            
        } else ZVAL_NULL(*member);
        
        pthreads_store_unlock(object TSRMLS_CC);
        
        return SUCCESS;
    }
   }
   
   return FAILURE;
} /* }}} */

/* {{{ copy store to hashtable */
void pthreads_store_tohash(pthreads_store store, HashTable *hash TSRMLS_DC) {

	zend_bool locked;
	if (store) {
	    /* php is reusing this hash and making things misbehave */
	    zend_hash_clean(hash);
	    
		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
			HashTable *stored = &store->table;
			HashPosition position;
			pthreads_storage *storage;

			for (zend_hash_internal_pointer_reset_ex(stored, &position);
				zend_hash_get_current_data_ex(stored, (void**) &storage, &position)==SUCCESS;
				zend_hash_move_forward_ex(stored, &position)) {	
			
				char *name;
				uint nlength;
				ulong idx;

				if (zend_hash_get_current_key_ex(stored, &name, &nlength, &idx, 0, &position)==HASH_KEY_IS_STRING) {
					if (name[0] != '$') { /* do not copy internal counter */
						char *rename = estrndup(name, nlength);
						{
							zval *pzval;

							MAKE_STD_ZVAL(pzval);
					
							if (pthreads_store_convert(storage, pzval TSRMLS_CC)!=SUCCESS) {
								ZVAL_NULL(pzval);
							} 
							
							zend_hash_update(hash, rename, nlength, &pzval, sizeof(zval), NULL);
						}
						efree(rename);
					}
				}
			}
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		}
	}
} /* }}} */

/* {{{ free store storage for a thread */
void pthreads_store_free(pthreads_store store TSRMLS_DC){
	if (store) {
		zend_bool locked;

		if (pthreads_lock_acquire(store->lock, &locked TSRMLS_CC)) {
			zend_hash_destroy(&store->table);
			pthreads_lock_release(store->lock, locked TSRMLS_CC);
		}
		pthreads_lock_free(store->lock TSRMLS_CC);
		free(store);
	}
} /* }}} */

/* {{{ Will storeize the zval into a newly allocated buffer which must be free'd by the caller */
static void pthreads_store_create(pthreads_storage *storage, zval *unstore, zend_bool complex TSRMLS_DC){					
	
	if (unstore) {
		/*
		* Make an exact copy of the store data for internal storage
		*/
		storage->length = 0;
		storage->exists = 0;
		storage->data = NULL;
		
		switch((storage->type = Z_TYPE_P(unstore))){
			case IS_NULL: { /* nothing to do */ } break;
			
			case IS_STRING: {
				if ((storage->length = Z_STRLEN_P(unstore))) {
					storage->data = (char*) malloc(storage->length+1);
					memcpy(
						storage->data, (const void*) Z_STRVAL_P(unstore), storage->length+1
					);
				}
			} break;
			
			case IS_BOOL:
			case IS_LONG: {
				storage->simple.lval = Z_LVAL_P(unstore);
			} break;

			case IS_RESOURCE: {
				if (complex) {
					pthreads_resource resource = malloc(sizeof(*resource));
					if (resource) {	
						resource->original = zend_list_find
							(Z_RESVAL_P(unstore), &resource->type);
						resource->ls = TSRMLS_C;
						
						storage->simple.lval = Z_RESVAL_P(unstore);
						storage->data = resource;
                        
						zend_list_addref(Z_RESVAL_P(unstore));
					}
				} else {
					storage->type = IS_NULL;
				}
			} break;
			
			case IS_DOUBLE:
			    storage->simple.dval = Z_DVAL_P(unstore);
			break;
			
			case IS_OBJECT:
			    if (instanceof_function(Z_OBJCE_P(unstore), zend_ce_closure TSRMLS_CC)) {
			        const zend_function *def = 
			            zend_get_closure_method_def(unstore TSRMLS_CC);
			        storage->type = IS_CLOSURE;
			        storage->data = 
			            (zend_function*) malloc(sizeof(zend_function));
			        memcpy(storage->data, def, sizeof(zend_function));
			        break;
			    }
			    
			case IS_ARRAY: {
				if (pthreads_store_tostring(unstore, (char**) &storage->data, &storage->length, complex TSRMLS_CC)==SUCCESS) {
					if (storage->type==IS_ARRAY) {
						storage->exists = zend_hash_num_elements(
						    Z_ARRVAL_P(unstore));
					}
				}
			} break;
			
			default: {
			    storage->exists = 0;
			}
		}
	}
}
/* }}} */

/* {{{ Will unstoreize data into the allocated zval passed */
static int pthreads_store_convert(pthreads_storage *storage, zval *pzval TSRMLS_DC){
	int result = SUCCESS;
	
	if (storage) {
		switch(storage->type) {
			case IS_STRING:
				if (storage->data && storage->length) {
					ZVAL_STRINGL(pzval, (char*)storage->data, storage->length, 1); 
				} else ZVAL_EMPTY_STRING(pzval);
			break;
			
			case IS_NULL: Z_TYPE_P(pzval) = IS_NULL; break;
			case IS_BOOL: ZVAL_BOOL(pzval, storage->simple.lval); break;
			case IS_LONG: ZVAL_LONG(pzval, storage->simple.lval); break;
			case IS_DOUBLE: ZVAL_DOUBLE(pzval, storage->simple.dval); break;
			case IS_RESOURCE: {	
				pthreads_resource resource = (pthreads_resource) storage->data;
				zend_rsrc_list_entry *original;
				if (resource->ls != TSRMLS_C && zend_hash_index_find(&PTHREADS_EG(resource->ls, regular_list), storage->simple.lval, (void**)&original)==SUCCESS) {
				
					zend_bool found = 0;
					ulong existing;
					{
						zend_rsrc_list_entry *search;
						HashPosition position;
						for(zend_hash_internal_pointer_reset_ex(&EG(regular_list), &position);
								zend_hash_get_current_data_ex(&EG(regular_list), (void**) &search, &position)==SUCCESS;
								zend_hash_move_forward_ex(&EG(regular_list), &position)) {

							if (search->ptr == original->ptr) {
								found=1;
								existing = position->h;
								break;
							}
						}
					}
					
					if (!found) {
					    zend_rsrc_list_entry create;
					    {
						    ulong created;
							
						    create.type = original->type;
						    create.ptr = original->ptr;
						    create.refcount = 1;
						    created=zend_hash_next_free_element(&EG(regular_list));

						    if (zend_hash_index_update(
							    &EG(regular_list), created, (void*) &create, sizeof(zend_rsrc_list_entry), (void**)&resource->copy
						    )==SUCCESS) {
							    ZVAL_RESOURCE(pzval, created);
							    pthreads_resources_keep(resource TSRMLS_CC);
						    } else ZVAL_NULL(pzval);
					    }
					} else {
						ZVAL_RESOURCE(pzval, existing);
						zend_list_addref(Z_RESVAL_P(pzval));
					}
				} else {
					ZVAL_RESOURCE(pzval, storage->simple.lval);
					zend_list_addref(Z_RESVAL_P(pzval));
				}
			} break;
			
			case IS_CLOSURE: {
			    char *name;
			    int name_len;
			    
			    zend_function *closure = (zend_function*) storage->data;
                
                pthreads_copy_function(closure);

#if PHP_VERSION_ID >= 50400
                zend_create_closure(pzval, closure, EG(scope), EG(This) TSRMLS_CC);
#else
                zend_create_closure(pzval, closure TSRMLS_CC);
#endif
			    
			    closure = (zend_function*) zend_get_closure_method_def(pzval TSRMLS_CC);
			    
			    name_len = spprintf(&name, 0, "Closure@%p", closure);
			    
			    zend_hash_update(
			        EG(function_table), name, name_len, (void**)closure, sizeof(zend_function), NULL);
			    
			    result = SUCCESS;
			    
			    efree(name);
			} break;
			
			case IS_ARRAY:
			case IS_OBJECT: {
				result = pthreads_store_tozval(
					pzval, 
					(char*) storage->data, 
					storage->length TSRMLS_CC
				);
				if (result == FAILURE) {
				    /* display error, do something ... ? */
				    Z_TYPE_P(pzval) = IS_NULL;
				}
			} break;
			
			default: {
			    Z_TYPE_P(pzval) = IS_NULL;
			}
		}	
	}
	return result;
}
/* }}} */

/* {{{ zval to string */
static int pthreads_store_tostring(zval *pzval, char **pstring, size_t *slength, zend_bool complex TSRMLS_DC) {
	int result = FAILURE;
	if (pzval && 
	    (Z_TYPE_P(pzval) != IS_NULL) && 
	    (Z_TYPE_P(pzval) != IS_OBJECT || pthreads_store_validate_object(&pzval TSRMLS_CC))) {
	    
		smart_str *psmart = (smart_str*) ecalloc(1, sizeof(smart_str));
		
		if (psmart) {
			if (!complex && 
			    (Z_TYPE_P(pzval) == IS_OBJECT || Z_TYPE_P(pzval) == IS_ARRAY)) {
				pthreads_store_remove_complex_recursive(&pzval TSRMLS_CC);
			}

			{
				if ((Z_TYPE_P(pzval) != IS_OBJECT) ||
					(Z_OBJCE_P(pzval)->serialize != zend_class_serialize_deny)) {
					php_serialize_data_t vars;
					PHP_VAR_SERIALIZE_INIT(vars);
					php_var_serialize(						
						psmart,
						&pzval, 
						&vars TSRMLS_CC
					);
					PHP_VAR_SERIALIZE_DESTROY(vars);
				}
			}

			*slength = psmart->len;
			if (psmart->len) {
				*pstring = malloc(psmart->len+1);
				if (*pstring) {
					memcpy(
						(char*) *pstring, (const void*) psmart->c, psmart->len
					);
					result = SUCCESS;
				}
			} else *pstring = NULL;
			smart_str_free(psmart);	
			efree(psmart);
		}
	} else {
	    *slength = 0;
	    *pstring = NULL;
	}
	return result;
} /* }}} */

/* {{{ string to zval */
static int pthreads_store_tozval(zval *pzval, char *pstring, size_t slength TSRMLS_DC) {
	int result = SUCCESS;
	ulong refcount = Z_REFCOUNT_P(pzval);
	if (pstring) {
		const unsigned char* pointer = (const unsigned char*) pstring;
		if (pointer) {
			/*
			* Populate PHP storage from pthreads_storage
			*/
			{
				php_unserialize_data_t vars;
				PHP_VAR_UNSERIALIZE_INIT(vars);
				if (!php_var_unserialize(&pzval, &pointer, pointer+slength, &vars TSRMLS_CC)) {
					result = FAILURE;
					zval_dtor(pzval);
				}							
				PHP_VAR_UNSERIALIZE_DESTROY(vars);
			}
			if (pzval && refcount)
				Z_SET_REFCOUNT_P(pzval, refcount);
		} else result = FAILURE;
	} else result = FAILURE;
	
	return result;
} /* }}} */

/* {{{ import members from one object (or array) into a pthreads object */
int pthreads_store_merge(zval *destination, zval *from, zend_bool overwrite TSRMLS_DC) {
    if (Z_TYPE_P(from) != IS_ARRAY && 
        Z_TYPE_P(from) != IS_OBJECT) {
        return FAILURE;
    }
    
    switch (Z_TYPE_P(from)) {
        case IS_OBJECT: {
            /* check for a suitable pthreads object */
            if (IS_PTHREADS_OBJECT(from)) {
                
                zend_bool locked[2] = {0, 0};
                PTHREAD pobjects[2] = {PTHREADS_FETCH_FROM(destination), PTHREADS_FETCH_FROM(from)};
                
                /* acquire destination lock */
                if (pthreads_lock_acquire(pobjects[0]->store->lock, &locked[0] TSRMLS_CC)) {
                    
                    /* acquire other lock */
                    if (pthreads_lock_acquire(pobjects[1]->store->lock, &locked[1] TSRMLS_CC)) {
                        
                        /* free to do what we want, everything locked */
                        
                        HashPosition position;
                        pthreads_storage *storage;
                        HashTable *tables[2] = {&pobjects[0]->store->table, &pobjects[1]->store->table};
                        
                        for (zend_hash_internal_pointer_reset_ex(tables[1], &position);
                             zend_hash_get_current_data_ex(tables[1], (void**)&storage, &position) == SUCCESS;
                             zend_hash_move_forward_ex(tables[1], &position)) {
                             
                             char *key = NULL;
                             zend_uint klen = 0;
                             zend_ulong idx = 0L;
                         
                            if (zend_hash_get_current_key_ex(tables[1], &key, &klen, &idx, 0, &position) == HASH_KEY_IS_STRING) {
                                
                                /* skip if not overwriting where the 
                                        entry exists in destination table */
                                if (!overwrite && zend_hash_exists(tables[0], key, klen)) {
                                    continue;
                                }
                                
                                /* skip resources */
                                if (storage->type != IS_RESOURCE) {

                                    /* copy storage */
                                    pthreads_storage copy;
                                    
                                    switch ((copy.type = storage->type)) {
                                        case IS_NULL: {
                                            copy.exists = 1;
                                        } break;
                                        
                                        case IS_STRING: {
                                            if ((copy.length = storage->length)) {
                                                copy.data = (char*) malloc(copy.length+1);
                                                memcpy(
                                                    copy.data, (const void*) storage->data, copy.length
                                                );
                                            }
                                        } break;
                                        
                                        case IS_BOOL:
                                        case IS_LONG: { copy.simple.lval = storage->simple.lval; } break;
                                        
                                        case IS_DOUBLE: { copy.simple.dval = storage->simple.dval; } break;
                                        
                                        case IS_OBJECT:
				                        case IS_ARRAY: {
				                            if ((copy.length = storage->length)) {
                                                copy.data = (char*) malloc(copy.length+1);
                                                memcpy(
                                                    copy.data, (const void*) storage->data, copy.length
                                                );
                                                
                                                if (copy.type == IS_ARRAY)
                                                    copy.exists = storage->exists;
                                            }
				                        } break;
				                        
				                        default: {  }
                                    }

                                    zend_hash_update(tables[0], key, klen, (void**) &copy, sizeof(pthreads_storage), NULL);
                                }
                            }
                        }
                        
                        pthreads_lock_release(pobjects[1]->store->lock, locked[1] TSRMLS_CC);
                    }
                    
                    pthreads_lock_release(pobjects[0]->store->lock, locked[0] TSRMLS_CC);
                    
                    return SUCCESS;
                    
                } else return FAILURE;
            }
        }
        
        /* fall through on purpose to handle normal objects and arrays */
        
        default: {
           zend_bool locked = 0;
           PTHREAD pobject = PTHREADS_FETCH_FROM(destination);
           
           if (pthreads_lock_acquire(pobject->store->lock, &locked TSRMLS_CC)) {
               HashPosition position;
               zval **pzval;
               zend_uint index = 0;
               HashTable *table = (Z_TYPE_P(from) == IS_ARRAY) ? Z_ARRVAL_P(from) : Z_OBJPROP_P(from);
               
               for (zend_hash_internal_pointer_reset_ex(table, &position);
                    zend_hash_get_current_data_ex(table, (void**)&pzval, &position) == SUCCESS;
                    zend_hash_move_forward_ex(table, &position)) {
                    char *key;
                    zend_uint klen;
                    zend_ulong idx;
                    
                    switch (zend_hash_get_current_key_ex(table, &key, &klen, &idx, 0, &position)) {
                        case HASH_KEY_IS_STRING: {
                            if (!overwrite && zend_hash_exists(&pobject->store->table, key, klen)) {
                                goto next;
                            }
                            
                            pthreads_store_write(
                                pobject->store, key, klen-1, pzval TSRMLS_CC);
                        } break;
                        
                        case HASH_KEY_IS_LONG: {
                            zval zkey;
                            
                            ZVAL_LONG(&zkey, idx);
                            
                            convert_to_string(&zkey);
                            
                            if (!overwrite && zend_hash_exists(&pobject->store->table, Z_STRVAL(zkey), Z_STRLEN(zkey))) {
                                zval_dtor(&zkey);
                                goto next;
                            }
                            
                            pthreads_store_write(
                                pobject->store, Z_STRVAL(zkey), Z_STRLEN(zkey), pzval TSRMLS_CC);
                            
                            zval_dtor(&zkey);
                        } break;
                    }
next:
                    index++;
               }
               
               pthreads_lock_release(pobject->store->lock, locked TSRMLS_CC);
           }
        } break;
    }
    
    return SUCCESS;
} /* }}} */

/* {{{ set resources to NULL for non-complex types; helper-function for pthreads_store_remove_complex_recursive */
static int pthreads_store_remove_complex(zval **pzval TSRMLS_DC) {
	if (Z_TYPE_PP(pzval) == IS_RESOURCE) {
		Z_TYPE_PP(pzval) = IS_NULL;
	}
	return pthreads_store_remove_complex_recursive(pzval TSRMLS_CC);
} /* }}} */

/* {{{ check a handle is valid before reading it */
static int pthreads_store_validate_object(zval **pzval TSRMLS_DC) {
	return (EG(objects_store).top > Z_OBJ_HANDLE_PP(pzval)) && (Z_OBJ_P(*pzval));
} /* }}} */

/* {{{ set corrupt objects (like mysqli after thread duplication) to NULL and recurse */
static int pthreads_store_remove_complex_recursive(zval **pzval TSRMLS_DC) {
	int is_temp;

	HashTable *thash = NULL;
	
	switch (Z_TYPE_PP(pzval)) {
		case IS_ARRAY:
			thash = Z_ARRVAL_PP(pzval);

		case IS_OBJECT:
			if (thash == NULL) {
				if (!pthreads_store_validate_object(pzval TSRMLS_CC)) {
					GC_REMOVE_ZVAL_FROM_BUFFER(*pzval);
					Z_TYPE_PP(pzval) = IS_NULL;
					return ZEND_HASH_APPLY_KEEP;
				}
				thash = Z_OBJDEBUG_PP(pzval, is_temp);
			}

			if (thash) {
				zend_hash_apply(thash, (apply_func_t)pthreads_store_remove_complex TSRMLS_CC);
			}
			
		break;
	}
	return ZEND_HASH_APPLY_KEEP;
} /* }}} */

/* {{{ Will free store element */
static void pthreads_store_storage_dtor (pthreads_storage *storage){
	if (storage) {
		switch (storage->type) {
		    case IS_CLOSURE:
			case IS_OBJECT:
			case IS_STRING:
			case IS_ARRAY:
			case IS_RESOURCE:
				if (storage->data) {
					free(storage->data);
				}
			break;
		}
	}
} /* }}} */

#endif
