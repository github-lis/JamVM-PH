/*
 * Copyright (C) 2003, 2004, 2005, 2006, 2007, 2008, 2009
 * Robert Lougher <rob@jamvm.org.uk>.
 *
 * This file is part of JamVM.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2,
 * or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301, USA.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/mman.h>
#include <dirent.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>


#include "jam.h"
#include "sig.h"
#include "thread.h"
#include "lock.h"
#include "hash.h"
#include "zip.h"
#include "class.h"
#include "interp.h"
#include "symbol.h"
#include "excep.h"

#define PREPARE(ptr) ptr
#define SCAVENGE(ptr) FALSE
#define FOUND(ptr1, ptr2) ptr2

#define HASHFILESIZE 4194304

static int verbose;
static char *bootpath;
static char *classpath;
static int max_cp_element_len;

/* Structures holding the boot loader classpath */
typedef struct bcp_entry {
    char *path;
    ZipFile *zip;
} BCPEntry;

static BCPEntry *bootclasspath;
static int bcp_entries;

/* Cached offsets of fields in java.lang.ref.Reference objects */
int ref_referent_offset = -1;
int ref_queue_offset;

/* Cached offset of vmdata field in java.lang.ClassLoader objects */
int ldr_vmdata_offset = -1;

/* Helper method to create a Package Object representing a
   package loaded by the boot loader */
static MethodBlock *vm_loader_create_package = NULL;
static Class *package_array_class;

/* hash table containing packages loaded by the boot loader */
#define PCKG_INITSZE 1<<6
static HashTable boot_packages;

/* Hashtable entry for each package defined by the boot loader */
typedef struct package_entry {
    int index;
    char name[0];
} PackageEntry;

static MethodBlock *ldr_new_unloader = NULL;
static int ldr_data_tbl_offset;

/* Instance of java.lang.Class for java.lang.Class */
Class *java_lang_Class = NULL;

/* Method table index of ClassLoader.loadClass - used when
   requesting a Java-level class loader to load a class.
   Cached on first use. */
static int loadClass_mtbl_idx = -1;

/* Method table index of finalizer method and ClassLoader.enqueue.
   Used by finalizer and reference handler threads */
int finalize_mtbl_idx;
int enqueue_mtbl_idx;

/* hash table containing classes loaded by the boot loader and
   internally created arrays */
#define CLASS_INITSZE 1<<8
static HashTable boot_classes;

/* Array large enough to hold all primitive classes -
 * access protected by boot_classes hash table lock */
#define MAX_PRIM_CLASSES 9
static Class *prim_classes[MAX_PRIM_CLASSES];

/* Bytecode for stub abstract method.  If it is invoked
   we'll get an abstract method error. */
static char abstract_method[] = {OPC_ABSTRACT_METHOD_ERROR};

//My variables
static char *NH_FILENAME = "HW_CLASSES";
static char *SEP = "\n";
static int 	is_persistent_classes = FALSE;
int class_to_save;
char *FILENAME_CLASSES_HT = "classes.ht";
char *FILENAME_CLASSES_NAMES = "classes.cn";
int testing_mode_classes = FALSE;
FILE *hash_file_classes;
FILE *name_file_classes;
FILE *statics_file_classes;
int pointer_file_exists_classes = FALSE;
int name_file_exists_classes = FALSE;
int classes_ht_initialized = FALSE;
int statics_file_initialized = FALSE;
LogLevel log_level;

struct stat st = {0};



static Class *addClassToHash(Class *class, Object *class_loader) {
    HashTable *table;
    Class *entry;

#define HASH(ptr) utf8Hash(CLASS_CB((Class *)ptr)->name)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2)

    if(class_loader == NULL) {
        table = &boot_classes;
    }
    else {
        Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

        if(vmdata == NULL) {
            objectLock(class_loader);
            vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

            if(vmdata == NULL) {
                if((vmdata = allocObject(ldr_new_unloader->class)) == NULL) {
                    objectUnlock(class_loader);
                    return NULL;
                }
                table = sysMalloc(sizeof(HashTable));

                initHashTable((*table), CLASS_INITSZE, TRUE);

                INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset) = table;
                INST_DATA(class_loader, Object*, ldr_vmdata_offset) = vmdata;

                objectUnlock(class_loader);
            }
        }

        table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
    }

    //NVM MODIFICATION
    //Adding entry to file if needed
    if(is_persistent_classes){
    	if(class_loader != NULL){
    		entry = NULL;
    		findOnlyHashEntry((*table), class, entry, TRUE);
    		if(entry == NULL){
    			hash_file_classes = fopen (FILENAME_CLASSES_HT, "a+b");
    			name_file_classes = fopen (FILENAME_CLASSES_NAMES, "a+b");
    			unsigned short string_lenght = strlen(CLASS_CB((Class*)class)->name) + 1;
    			fwrite(&string_lenght, sizeof(unsigned short), 1, name_file_classes);
    			fwrite(CLASS_CB((Class*)class)->name, sizeof(char), string_lenght, name_file_classes);
    			fwrite(&class, sizeof(Class*), 1, hash_file_classes);
    			fclose(hash_file_classes);
    			fclose(name_file_classes);
    			classes_ht_initialized = TRUE;
    		}
    	}
    }
    //END OF MODIFICATION


    /* Add if absent, no scavenge, locked */
    findHashEntry((*table), class, entry, TRUE, FALSE, TRUE);
	msync(table, HASHFILESIZE, MS_SYNC);

    return entry;
}

static void prepareClass(Class *class) {

	ClassBlock *cb = CLASS_CB(class);


	if(cb->name == SYMBOL(java_lang_Class)) {
		java_lang_Class = class->class = class;
		cb->flags |= CLASS_CLASS;
	} else {
		if(java_lang_Class == NULL){

			findSystemClass0(SYMBOL(java_lang_Class));
		}
		class->class = java_lang_Class;
	}
}
//NVM method to check if a class is already on the hash table
Class *searchClass(char *classname, char *data, int offset, int len, Object *class_loader){
	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);
	if(vmdata == NULL){
		return NULL;
	}
	u2 major_version, minor_version, this_idx, super_idx;
	unsigned char *ptr = (unsigned char *)data + offset;
	int cp_count, intf_count, i;
	u2 attr_count;
	u4 magic;

	ConstantPool *constant_pool;
	ClassBlock *classblock;
	Class *class, *found;
	Class **interfaces;

	READ_U4(magic, ptr, len);

	if(magic != 0xcafebabe) {
		signalException(java_lang_ClassFormatError, "bad magic");
		return NULL;
	}

	READ_U2(minor_version, ptr, len);
	READ_U2(major_version, ptr, len);


	if((class = allocClass()) == NULL)
		return NULL;

	classblock = CLASS_CB(class);
	READ_U2(cp_count, ptr, len);



	constant_pool = &classblock->constant_pool;
	constant_pool->type = sysMalloc(cp_count);
	constant_pool->info = sysMalloc(cp_count*sizeof(ConstantPoolEntry));



	for(i = 1; i < cp_count; i++) {
		u1 tag;

		READ_U1(tag, ptr, len);
		CP_TYPE(constant_pool,i) = tag;

		switch(tag) {
		case CONSTANT_Class:
		case CONSTANT_String:
			READ_INDEX(CP_INFO(constant_pool,i), ptr, len);
			break;

		case CONSTANT_Fieldref:
		case CONSTANT_Methodref:
		case CONSTANT_NameAndType:
		case CONSTANT_InterfaceMethodref:
		{
			u2 idx1, idx2;

			READ_INDEX(idx1, ptr, len);
			READ_INDEX(idx2, ptr, len);
			CP_INFO(constant_pool,i) = (idx2<<16)+idx1;
			break;
		}

		case CONSTANT_Float:
		case CONSTANT_Integer:
			READ_U4(CP_INFO(constant_pool,i), ptr, len);
			break;

		case CONSTANT_Long:
			READ_U8(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
			CP_TYPE(constant_pool,++i) = 0;
			break;

		case CONSTANT_Double:
			READ_DBL(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
			CP_TYPE(constant_pool,++i) = 0;
			break;

		case CONSTANT_Utf8:
		{
			int length;
			char *buff, *utf8;

			READ_U2(length, ptr, len);
			buff = sysMalloc(length+1);

			memcpy(buff, ptr, length);
			buff[length] = '\0';
			ptr += length;


			//XXX NVM MODIFICATION - save utf8 entries of classes with classloaders
			if(class_to_save){
				CP_INFO(constant_pool,i) = (uintptr_t) (utf8 = newUtf8Save(buff));
			}
			else{
				CP_INFO(constant_pool,i) = (uintptr_t) (utf8 = newUtf8(buff));
			}


			if(utf8 != buff)
				sysFree(buff);

			break;
		}

		default:
			signalException(java_lang_ClassFormatError,
					"bad constant pool tag");
			return NULL;
		}
	}


	/* Set count after constant pool has been initialised -- it is now
	       safe to be scanned by GC */
	classblock->constant_pool_count = cp_count;

	READ_U2(classblock->access_flags, ptr, len);

	READ_TYPE_INDEX(this_idx, constant_pool, CONSTANT_Class, ptr, len);
	classblock->name = CP_UTF8(constant_pool, CP_CLASS(constant_pool, this_idx));

	// Search class on classes hash table
	HashTable *classes_table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
	Class *result = NULL;
	findHashEntry((*classes_table), class, result, FALSE, FALSE, TRUE);

	// Unit tests
	if(testing_mode_classes) {
		if(result == NULL){
			log_test_results("searchClass_foundClass", FALSE);
		}
		else{
			log_test_results("searchClass_foundClass", TRUE);
		}
	}
	return result;

}

Class *defineClass(char *classname, char *data, int offset, int len,
                   Object *class_loader) {

	//NVM Variables
	int reload = FALSE;
	Class *result = NULL;


	//NVM MODIFICATION
	if(is_persistent_classes){
		if(class_loader != NULL){
			class_to_save = TRUE;
			result = searchClass(classname, data, offset, len, class_loader);
			if(result == NULL){
				reload = FALSE;
			}
			else{
				reload = TRUE;
			}
		}
		else{
			class_to_save = FALSE;
			reload = FALSE;
		}
	}
	//END OF MODIFICATION



    u2 major_version, minor_version, this_idx, super_idx;
    unsigned char *ptr = (unsigned char *)data + offset;
    int cp_count, intf_count, i;
    u2 attr_count;
    u4 magic;

    ConstantPool *constant_pool;
    ClassBlock *classblock;
    Class *class, *found;
    Class **interfaces;

    READ_U4(magic, ptr, len);

    if(magic != 0xcafebabe) {
       signalException(java_lang_ClassFormatError, "bad magic");
       return NULL;
    }

    READ_U2(minor_version, ptr, len);
    READ_U2(major_version, ptr, len);


    //NVM MOFICIATION (FIRST IF CLAUSE. ELSE IS VOLATILE FLUX)
    if(reload == TRUE){
    	class = result;
    }
    else{
    	if((class = allocClass()) == NULL)
    		return NULL;
    }
    //END OF MODIFICATION


    classblock = CLASS_CB(class);
    READ_U2(cp_count, ptr, len);



    constant_pool = &classblock->constant_pool;
    constant_pool->type = sysMalloc(cp_count);
    constant_pool->info = sysMalloc(cp_count*sizeof(ConstantPoolEntry));



    for(i = 1; i < cp_count; i++) {
        u1 tag;

        READ_U1(tag, ptr, len);
        CP_TYPE(constant_pool,i) = tag;

        switch(tag) {
           case CONSTANT_Class:
           case CONSTANT_String:
               READ_INDEX(CP_INFO(constant_pool,i), ptr, len);
               break;

           case CONSTANT_Fieldref:
           case CONSTANT_Methodref:
           case CONSTANT_NameAndType:
           case CONSTANT_InterfaceMethodref:
           {
               u2 idx1, idx2;

               READ_INDEX(idx1, ptr, len);
               READ_INDEX(idx2, ptr, len);
               CP_INFO(constant_pool,i) = (idx2<<16)+idx1;
               break;
           }

           case CONSTANT_Float:
           case CONSTANT_Integer:
               READ_U4(CP_INFO(constant_pool,i), ptr, len);
               break;

           case CONSTANT_Long:
               READ_U8(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
               CP_TYPE(constant_pool,++i) = 0;
               break;

           case CONSTANT_Double:
               READ_DBL(*(u8 *)&(CP_INFO(constant_pool,i)), ptr, len);
               CP_TYPE(constant_pool,++i) = 0;
               break;

           case CONSTANT_Utf8:
           {
               int length;
               char *buff, *utf8;

               READ_U2(length, ptr, len);
               buff = sysMalloc(length+1);

               memcpy(buff, ptr, length);
               buff[length] = '\0';
               ptr += length;

               //NVM MODIFICATION
               //Changed this part to save utf8 entries of classes with classloaders
               if(class_to_save){
            	   CP_INFO(constant_pool,i) = (uintptr_t) (utf8 = newUtf8Save(buff));
               }
               else{
            	   CP_INFO(constant_pool,i) = (uintptr_t) (utf8 = newUtf8(buff));
               }
               //END OF MODIFICATION

               if(utf8 != buff)
                   sysFree(buff);

               break;
           }

           default:
               signalException(java_lang_ClassFormatError,
                               "bad constant pool tag");
               return NULL;
        }
    }


    /* Set count after constant pool has been initialised -- it is now
       safe to be scanned by GC */
    classblock->constant_pool_count = cp_count;

    READ_U2(classblock->access_flags, ptr, len);

    READ_TYPE_INDEX(this_idx, constant_pool, CONSTANT_Class, ptr, len);
    classblock->name = CP_UTF8(constant_pool, CP_CLASS(constant_pool, this_idx));

    if(classname && strcmp(classblock->name, classname) != 0) {
        signalException(java_lang_NoClassDefFoundError,
                        "class file has wrong name");
        return NULL;
    }

    prepareClass(class);



    if(classblock->name == SYMBOL(java_lang_Object)) {
        READ_U2(super_idx, ptr, len);
        if(super_idx) {
           signalException(java_lang_ClassFormatError, "Object has super");
           return NULL;
        }
        classblock->super_name = NULL;
    } else {
        READ_TYPE_INDEX(super_idx, constant_pool, CONSTANT_Class, ptr, len);
        classblock->super_name = CP_UTF8(constant_pool, CP_CLASS(constant_pool, super_idx));
    }

    // XXX NVM MODIFICATION
    if(is_persistent_classes){
    	if(class_loader != NULL){
    		int file_exists;
    		//Check if fields folder exists, if not create it
    		if (stat("fields", &st) == -1) {
    			mkdir("fields", 0700);
    		}
    		//Malloc the size of the classname plus fields/ (7) plus .fld (4) plus \0 (1)
    		char *file_name = (char*)calloc(1, sizeof(classblock->name)+12);
    		strcat(file_name,"fields/");
    		strcat(file_name, classblock->name);
    		strcat(file_name, ".fld\0");

    		if(access(file_name, F_OK) == -1) {

    			if(testing_mode_classes){
    				log_level = DEBUG;
    				char log[100];
    				sprintf(log, "Creating field file: %s", file_name);
    				log(log_level, log);
    			}

    			statics_file_classes = fopen(file_name, "wr+b");
    			fclose(statics_file_classes);
    		}



    		//Unit Tests
    		if(testing_mode_classes){
    			if(access(file_name, F_OK) != -1) {
    				file_exists = TRUE;
    			}
    			else{
    				file_exists = FALSE;
    			}
    			log_test_results("defineClass_fileExists", file_exists);
    		}
    	}
    }
    //END OF MODIFICATION





    classblock->class_loader = class_loader;

    READ_U2(intf_count = classblock->interfaces_count, ptr, len);
    interfaces = classblock->interfaces = sysMalloc(intf_count * sizeof(Class *));

    memset(interfaces, 0, intf_count * sizeof(Class *));
    for(i = 0; i < intf_count; i++) {
       u2 index;
       READ_TYPE_INDEX(index, constant_pool, CONSTANT_Class, ptr, len);
       interfaces[i] = resolveClass(class, index, FALSE);
       if(exceptionOccurred())
           return NULL;
    }

    READ_U2(classblock->fields_count, ptr, len);
    classblock->fields = sysMalloc(classblock->fields_count * sizeof(FieldBlock));



    for(i = 0; i < classblock->fields_count; i++) {
        u2 name_idx, type_idx;

        READ_U2(classblock->fields[i].access_flags, ptr, len);
        READ_TYPE_INDEX(name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
        READ_TYPE_INDEX(type_idx, constant_pool, CONSTANT_Utf8, ptr, len);
        classblock->fields[i].name = CP_UTF8(constant_pool, name_idx);
        classblock->fields[i].type = CP_UTF8(constant_pool, type_idx);
        classblock->fields[i].annotations = NULL;
        classblock->fields[i].signature = NULL;
        classblock->fields[i].constant = 0;

        READ_U2(attr_count, ptr, len);
        for(; attr_count != 0; attr_count--) {
            u2 attr_name_idx;
            char *attr_name;
            u4 attr_length;

            READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
            attr_name = CP_UTF8(constant_pool, attr_name_idx);
            READ_U4(attr_length, ptr, len);

            if(attr_name == SYMBOL(ConstantValue)) {
                READ_INDEX(classblock->fields[i].constant, ptr, len);
            } else
                if(attr_name == SYMBOL(Signature)) {
                    u2 signature_idx;
                    READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
                    classblock->fields[i].signature = CP_UTF8(constant_pool, signature_idx);
                } else
                    if(attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
                        classblock->fields[i].annotations = sysMalloc(sizeof(AnnotationData));
                        classblock->fields[i].annotations->len = attr_length;
                        classblock->fields[i].annotations->data = sysMalloc(attr_length);
                        memcpy(classblock->fields[i].annotations->data, ptr, attr_length);
                        ptr += attr_length;
                    } else
                        ptr += attr_length;
        }
    }

    READ_U2(classblock->methods_count, ptr, len);

    classblock->methods = sysMalloc(classblock->methods_count * sizeof(MethodBlock));

    memset(classblock->methods, 0, classblock->methods_count * sizeof(MethodBlock));

    for(i = 0; i < classblock->methods_count; i++) {
        MethodBlock *method = &classblock->methods[i];
        MethodAnnotationData annos;
        u2 name_idx, type_idx;

        memset(&annos, 0, sizeof(MethodAnnotationData));

        READ_U2(method->access_flags, ptr, len);
        READ_TYPE_INDEX(name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
        READ_TYPE_INDEX(type_idx, constant_pool, CONSTANT_Utf8, ptr, len);

        method->name = CP_UTF8(constant_pool, name_idx);
        method->type = CP_UTF8(constant_pool, type_idx);

        READ_U2(attr_count, ptr, len);
        for(; attr_count != 0; attr_count--) {
            u2 attr_name_idx;
            char *attr_name;
            u4 attr_length;

            READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
            READ_U4(attr_length, ptr, len);
            attr_name = CP_UTF8(constant_pool, attr_name_idx);

            if(attr_name == SYMBOL(Code)) {
                u4 code_length;
                u2 code_attr_cnt;
                int j;

                READ_U2(method->max_stack, ptr, len);
                READ_U2(method->max_locals, ptr, len);

                READ_U4(code_length, ptr, len);
                method->code = sysMalloc(code_length);
                memcpy(method->code, ptr, code_length);
                ptr += code_length;

                method->code_size = code_length;

                READ_U2(method->exception_table_size, ptr, len);
                method->exception_table = sysMalloc(method->exception_table_size*sizeof(ExceptionTableEntry));

                for(j = 0; j < method->exception_table_size; j++) {
                    ExceptionTableEntry *entry = &method->exception_table[j];

                    READ_U2(entry->start_pc, ptr, len);
                    READ_U2(entry->end_pc, ptr, len);
                    READ_U2(entry->handler_pc, ptr, len);
                    READ_U2(entry->catch_type, ptr, len);
                }

                READ_U2(code_attr_cnt, ptr, len);
                for(; code_attr_cnt != 0; code_attr_cnt--) {
                    u2 attr_name_idx;
                    u4 attr_length;

                    READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
                    attr_name = CP_UTF8(constant_pool, attr_name_idx);
                    READ_U4(attr_length, ptr, len);

                    if(attr_name == SYMBOL(LineNumberTable)) {
                        READ_U2(method->line_no_table_size, ptr, len);
                        method->line_no_table = sysMalloc(method->line_no_table_size*sizeof(LineNoTableEntry));

                        for(j = 0; j < method->line_no_table_size; j++) {
                            LineNoTableEntry *entry = &method->line_no_table[j];

                            READ_U2(entry->start_pc, ptr, len);
                            READ_U2(entry->line_no, ptr, len);
                        }
                    } else
                        ptr += attr_length;
                }
            } else
                if(attr_name == SYMBOL(Exceptions)) {
                    int j;

                    READ_U2(method->throw_table_size, ptr, len);
                    method->throw_table = sysMalloc(method->throw_table_size*sizeof(u2));
                    for(j = 0; j < method->throw_table_size; j++) {
                        READ_U2(method->throw_table[j], ptr, len);
                    }
                } else
                    if(attr_name == SYMBOL(Signature)) {
                        u2 signature_idx;
                        READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
                        method->signature = CP_UTF8(constant_pool, signature_idx);
                    } else
                        if(attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
                            annos.annotations = sysMalloc(sizeof(AnnotationData));
                            annos.annotations->len = attr_length;
                            annos.annotations->data = sysMalloc(attr_length);
                            memcpy(annos.annotations->data, ptr, attr_length);
                            ptr += attr_length;
                        } else
                            if(attr_name == SYMBOL(RuntimeVisibleParameterAnnotations)) {
                                annos.parameters = sysMalloc(sizeof(AnnotationData));
                                annos.parameters->len = attr_length;
                                annos.parameters->data = sysMalloc(attr_length);
                                memcpy(annos.parameters->data, ptr, attr_length);
                                ptr += attr_length;
                            } else
                                if(attr_name == SYMBOL(AnnotationDefault)) {
                                    annos.dft_val = sysMalloc(sizeof(AnnotationData));
                                    annos.dft_val->len = attr_length;
                                    annos.dft_val->data = sysMalloc(attr_length);
                                    memcpy(annos.dft_val->data, ptr, attr_length);
                                    ptr += attr_length;
                                } else
                                    ptr += attr_length;
        }
        if(annos.annotations != NULL || annos.parameters != NULL
                                     || annos.dft_val != NULL) {
            method->annotations = sysMalloc(sizeof(MethodAnnotationData));
            memcpy(method->annotations, &annos, sizeof(MethodAnnotationData));
        }
    }

    READ_U2(attr_count, ptr, len);
    for(; attr_count != 0; attr_count--) {
        u2 attr_name_idx;
        char *attr_name;
        u4 attr_length;

        READ_TYPE_INDEX(attr_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
        attr_name = CP_UTF8(constant_pool, attr_name_idx);
        READ_U4(attr_length, ptr, len);

        if(attr_name == SYMBOL(SourceFile)) {
            u2 file_name_idx;
            READ_TYPE_INDEX(file_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
            classblock->source_file_name = CP_UTF8(constant_pool, file_name_idx);
        } else
            if(attr_name == SYMBOL(InnerClasses)) {
                int j, size;
                READ_U2(size, ptr, len);
                {
                    u2 inner_classes[size];
                    for(j = 0; j < size; j++) {
                        int inner, outer;
                        READ_TYPE_INDEX(inner, constant_pool, CONSTANT_Class, ptr, len);
                        READ_TYPE_INDEX(outer, constant_pool, CONSTANT_Class, ptr, len);

                        if(inner == this_idx) {
                            int inner_name_idx;

                            /* A member class doesn't have an EnclosingMethod attribute, so set
                               the enclosing class to be the same as the declaring class */
                            if(outer)
                                classblock->declaring_class = classblock->enclosing_class = outer;

                            READ_TYPE_INDEX(inner_name_idx, constant_pool, CONSTANT_Utf8, ptr, len);
                            if(inner_name_idx == 0)
                                classblock->flags |= ANONYMOUS;

                            READ_U2(classblock->inner_access_flags, ptr, len);
                        } else {
                            ptr += 4;
                            if(outer == this_idx)
                                inner_classes[classblock->inner_class_count++] = inner;
                        }
                    }

                    if(classblock->inner_class_count) {
                        classblock->inner_classes = sysMalloc(classblock->inner_class_count*sizeof(u2));
                        memcpy(classblock->inner_classes, &inner_classes[0],
                                                          classblock->inner_class_count*sizeof(u2));
                    }
                }
            } else
                if(attr_name == SYMBOL(EnclosingMethod)) {
                    READ_TYPE_INDEX(classblock->enclosing_class, constant_pool, CONSTANT_Class, ptr, len);
                    READ_TYPE_INDEX(classblock->enclosing_method, constant_pool, CONSTANT_NameAndType, ptr, len);
                } else
                    if(attr_name == SYMBOL(Signature)) {
                        u2 signature_idx;
                        READ_TYPE_INDEX(signature_idx, constant_pool, CONSTANT_Utf8, ptr, len);
                        classblock->signature = CP_UTF8(constant_pool, signature_idx);
                    } else
                        if(attr_name == SYMBOL(Synthetic))
                            classblock->access_flags |= ACC_SYNTHETIC;
                        else
                            if(attr_name == SYMBOL(RuntimeVisibleAnnotations)) {
                                classblock->annotations = sysMalloc(sizeof(AnnotationData));
                                classblock->annotations->len = attr_length;
                                classblock->annotations->data = sysMalloc(attr_length);
                                memcpy(classblock->annotations->data, ptr, attr_length);
                                ptr += attr_length;
                            } else
                                ptr += attr_length;
    }

    classblock->super = super_idx ? resolveClass(class, super_idx, FALSE) : NULL;

    if(exceptionOccurred())
       return NULL;

    classblock->state = CLASS_LOADED;

    if((found = addClassToHash(class, class_loader)) != class) {
        classblock->flags = CLASS_CLASH;
        if(class_loader != NULL) {
            signalException(java_lang_LinkageError, "duplicate class definition");
            return NULL;
        }
        return found;
    }

    return class;
}

Class *createArrayClass(char *classname, Object *class_loader) {
    ClassBlock *elem_cb, *classblock;
    Class *class, *found = NULL;
    int len = strlen(classname);

    if((class = allocClass()) == NULL)
        return NULL;

    classblock = CLASS_CB(class);

    classblock->name = copyUtf8(classname);
    classblock->super_name = SYMBOL(java_lang_Object);
    classblock->super = findSystemClass0(SYMBOL(java_lang_Object));
    classblock->method_table = CLASS_CB(classblock->super)->method_table;

    classblock->interfaces_count = 2;
    classblock->interfaces = sysMalloc(sizeof(Class*) * 2);
    classblock->interfaces[0] = findSystemClass0(SYMBOL(java_lang_Cloneable));
    classblock->interfaces[1] = findSystemClass0(SYMBOL(java_io_Serializable));

    classblock->state = CLASS_ARRAY;

    /* Find the array element class and the dimension --
       this is used to speed up type checking (instanceof) */

    if(classname[1] == '[') {
        Class *comp_class = findArrayClassFromClassLoader(classname + 1,
                                                          class_loader);

        if(comp_class == NULL)
            goto error;

        classblock->element_class = CLASS_CB(comp_class)->element_class;
        classblock->dim = CLASS_CB(comp_class)->dim + 1;
    } else {
        if(classname[1] == 'L') {
            char element_name[len - 2];

            memcpy(element_name, classname + 2, len - 3);
            element_name[len - 3] = '\0';

            classblock->element_class = findClassFromClassLoader(element_name,
                                                                 class_loader);
        } else
            classblock->element_class = findPrimitiveClass(classname[1]);

        if(classblock->element_class == NULL)
            goto error;

         classblock->dim = 1;
    }

    elem_cb = CLASS_CB(classblock->element_class);

    /* The array's classloader is the loader of the element class */
    classblock->class_loader = elem_cb->class_loader;

    /* The array's visibility (i.e. public, etc.) is that of the element */
    classblock->access_flags = (elem_cb->access_flags & ~ACC_INTERFACE) |
                               ACC_FINAL | ACC_ABSTRACT;

    prepareClass(class);

    if((found = addClassToHash(class, classblock->class_loader)) == class) {
        if(verbose)
            jam_printf("[Created array class %s]\n", classname);
        return class;
    }

error:
    classblock->flags = CLASS_CLASH;
    return found;
}

Class *createPrimClass(char *classname, int index) {
    Class *class;
    ClassBlock *classblock;

    if((class = allocClass()) == NULL)
        return NULL;

    classblock = CLASS_CB(class);
    classblock->name = classname;
    classblock->state = CLASS_PRIM + index;
    classblock->access_flags = ACC_PUBLIC | ACC_FINAL | ACC_ABSTRACT;

    prepareClass(class);

    lockHashTable(boot_classes);
    if(prim_classes[index] == NULL)
        prim_classes[index] = class;
    unlockHashTable(boot_classes);

    if(verbose)
        jam_printf("[Created primitive class %s]\n", classname);

    return prim_classes[index];
}

/* Layout the instance data.

   The object layout places 64-bit fields on a double-word boundary
   as on some architectures this leads to better performance (and
   misaligned loads/store may cause traps).

   Reference fields are also placed together as these must be scanned
   during GC, and placing them together reduces the number of entries
   required in the reference offsets list.

   Double/long fields are layed out first, then references and finally
   int-sized fields.  When padding is needed for 64-bit fields we try
   to place an int-sized field, and only leave a hole when no int-sized
   fields are available */

void prepareFields(Class *class) {
    ClassBlock *cb = CLASS_CB(class);
    Class *super = (cb->access_flags & ACC_INTERFACE) ? NULL
                                                      : cb->super;

    RefsOffsetsEntry *spr_rfs_offsts_tbl = NULL;
    int spr_rfs_offsts_sze = 0;

    FieldBlock *ref_head = NULL;
    FieldBlock *int_head = NULL;
    FieldBlock *dbl_head = NULL;

    int field_offset = sizeof(Object);
    int refs_start_offset = 0;
    int refs_end_offset = 0;
    int i;

    if(super != NULL) {
        field_offset = CLASS_CB(super)->object_size;
        spr_rfs_offsts_sze = CLASS_CB(super)->refs_offsets_size;
        spr_rfs_offsts_tbl = CLASS_CB(super)->refs_offsets_table;
    }

    /* Initialise static fields to default value, and separate
       instance fields into three linked lists, holding
       int-sized fields, double-sized fields and reference
       fields */

    for(i = 0; i < cb->fields_count; i++) {
        FieldBlock *fb = &cb->fields[i];

        if(fb->access_flags & ACC_STATIC){
        	fb->u.static_value.l = 0;


        	//XXX NVM MODIFICATION
        	if(is_persistent_classes){
        		if(CLASS_CB(class)->class_loader != NULL){
        			// Calloc the size of the classname plus fields/ (7) plus .fld (4) plus \0 (1)
        			char *file_name = (char*)calloc(1, strlen(CLASS_CB(class)->name) + 12);
        			strcat(file_name,"fields/");
        			strcat(file_name, CLASS_CB(class)->name);
        			strcat(file_name, ".fld\0");

        			if(testing_mode_classes){
        				log_level = DEBUG;
        				char log[100];
        				sprintf(log, "Initializing variable from field file: %s", file_name);
        				log(log_level, log);
        			}

        			// FIELD FILE STRUCTURE
        			// HEADER (number of chars) | VAR NAME | POINTER TO VALUE | PRIMITIVE BOOL | VALUE

        			// Reading field file
        			statics_file_classes = fopen(file_name, "r+b");
        			size_t len = 0;
        			unsigned short string_length;
        			long long *pointer;
        			void *pnt;
        			long long value = 0;
        			int reload_variable = FALSE;
        			int primitive = FALSE;

        			len = fread(&string_length, sizeof(unsigned short), 1, statics_file_classes);

        			if(testing_mode_classes){
        				log_level = DEBUG;
        				char log[100];
        				sprintf(log, "Variable: %s", fb->name);
        				log(log_level, log);
        			}

        			while (len == 1) {
        				char *var_name = (char*)calloc(1, (string_length * sizeof(char)) + 1);
        				fread(var_name, sizeof(char), string_length, statics_file_classes);
        			    var_name[string_length] = 0;

        			    // Found match, reassign value
        			    if(strcmp(var_name, fb->name) == 0){
        			    	reload_variable = TRUE;

        			    	pointer = &(fb->u.static_value.l);
        			    	fwrite(&pointer, sizeof(void *), 1, statics_file_classes);

        			    	fread(&primitive, sizeof(int), 1, statics_file_classes);

        			    	if(primitive == TRUE){
        			    		fread(&value, sizeof(long long), 1, statics_file_classes);
        			    		fb->u.static_value.l = value;
        			    	}
        			    	else{
        			    		fread(&pnt, sizeof(void *), 1, statics_file_classes);
        			    		fb->u.static_value.p = pnt;
        			    	}

        			    	if(testing_mode_classes){
        			    		log_level = DEBUG;
        			    		char log[100];
        			    		if(primitive == TRUE){
        			    			sprintf(log, "Found match in field file for variable %s. Assigning value %x", var_name, value);
        			    		}
        			    		else{
        			    			sprintf(log, "Found match in field file for variable %s. Assigning value %x", var_name, pnt);
        			    		}
        			    		log(log_level, log);
        			    	}

        			    	// Stop reading the file
        			    	break;
        			    }
        			    else{

        			    	// Continue to advance the read pointer
        			    	fread(&pointer, sizeof(void *), 1, statics_file_classes);

        			    	fread(&primitive, sizeof(int), 1, statics_file_classes);

        			    	if(primitive == TRUE){
        			    		fread(&value, sizeof(long long), 1, statics_file_classes);
        			    	}
        			    	else{
        			    		fread(&pnt, sizeof(void *), 1, statics_file_classes);
        			    	}
        			    }

        			    // Read next file entry
        				len = fread(&string_length, sizeof(unsigned short), 1, statics_file_classes);

        			}

        			fclose(statics_file_classes);

        			// Primitive types are 1 char long
        			if((strlen(fb->type) == 1) && (fb->type[0] != '[')){
        				primitive = TRUE;
        			}
        			else{
        				primitive = FALSE;
        			}


        			// Adding entry to field file
        			if(reload_variable == FALSE){
        				statics_file_classes = fopen(file_name, "a+b");

        				string_length = strlen(fb->name);
        				fwrite(&string_length, sizeof(unsigned short), 1, statics_file_classes);

        				fwrite(fb->name, sizeof(char), string_length, statics_file_classes);

        				pointer = &(fb->u.static_value.l);
        				fwrite(&pointer, sizeof(void *), 1, statics_file_classes);

        				fwrite(&primitive, sizeof(int), 1, statics_file_classes);

        				if(primitive == TRUE){
        					fwrite(&value, sizeof(long long), 1, statics_file_classes);
        				}
        				else{
        					fwrite(&value, sizeof(void *), 1, statics_file_classes);
        				}
        				fclose(statics_file_classes);

        				if(testing_mode_classes){
        					log_level = DEBUG;
        					char log[100];
        					sprintf(log,"Added entries to field file for variable: %s", fb->name);
        					log(log_level, log);
        				}

        			}

        			// UNIT TESTS
        			if(testing_mode_classes){
        				log_test_results("pepareFields_reloadVariable", reload_variable);
        			}
        		}
        	}
        	// END OF MODIFICATION
        }

        else {
            FieldBlock **list;

            if(fb->type[0] == 'L' || fb->type[0] == '[')
                list = &ref_head;
            else
                if(fb->type[0] == 'J' || fb->type[0] == 'D')
                    list = &dbl_head;
                else
                    list = &int_head;

            fb->u.static_value.p = *list;
            *list = fb;
        }

        fb->class = class;
    }

    /* Layout the double-sized fields.  If padding is required,
       use the first int-sized field (int_list head), or leave
       a hole if no int-fields */

    if(dbl_head != NULL) {
        if(field_offset & 0x7) {
            if(int_head != NULL) {
                FieldBlock *fb = int_head;
                int_head = int_head->u.static_value.p;
                fb->u.offset = field_offset;
            }
            field_offset += 4;
        }

        do {
            FieldBlock *fb = dbl_head;
            dbl_head = dbl_head->u.static_value.p;
            fb->u.offset = field_offset;
            field_offset += 8;
        } while(dbl_head != NULL);
    }

    /* Layout the reference fields.  If padding is required,
       use an int-sized field (int_list head), or leave
       a hole if no int-fields remaining */

    if(ref_head != NULL) {
        if(sizeof(Object*) == 8 && field_offset & 0x7) {
            if(int_head != NULL) {
                FieldBlock *fb = int_head;
                int_head = int_head->u.static_value.p;
                fb->u.offset = field_offset;
            }
            field_offset += 4;
        }

        refs_start_offset = field_offset;

        do {
            FieldBlock *fb = ref_head;
            ref_head = ref_head->u.static_value.p;
            fb->u.offset = field_offset;
            field_offset += sizeof(Object*);
        } while(ref_head != NULL);

        refs_end_offset = field_offset;
    }

    /* Layout the remaining int-sized fields */

    while(int_head != NULL) {
        FieldBlock *fb = int_head;
        int_head = int_head->u.static_value.p;
        fb->u.offset = field_offset;
        field_offset += 4;
    }

   cb->object_size = field_offset;

   /* Construct the reference offsets list.  This is used to speed up
      scanning of an objects references during the mark phase of GC.
      If possible, merge the entry with the previous entry */

   if(refs_start_offset) {
       if(spr_rfs_offsts_sze > 0 && spr_rfs_offsts_tbl[spr_rfs_offsts_sze-1].end
                                           == refs_start_offset) {

           cb->refs_offsets_size = spr_rfs_offsts_sze;
           refs_start_offset = spr_rfs_offsts_tbl[spr_rfs_offsts_sze-1].start;
       } else
           cb->refs_offsets_size = spr_rfs_offsts_sze + 1;

      cb->refs_offsets_table = sysMalloc(cb->refs_offsets_size *
                                         sizeof(RefsOffsetsEntry));

      memcpy(cb->refs_offsets_table, spr_rfs_offsts_tbl,
             spr_rfs_offsts_sze * sizeof(RefsOffsetsEntry));

      cb->refs_offsets_table[cb->refs_offsets_size-1].start = refs_start_offset;
      cb->refs_offsets_table[cb->refs_offsets_size-1].end = refs_end_offset;
   } else {
       cb->refs_offsets_size = spr_rfs_offsts_sze;
       cb->refs_offsets_table = spr_rfs_offsts_tbl;
   }
}

#define MRNDA_CACHE_SZE 10

#define resizeMTable(method_table, method_table_size, miranda, count)  \
{                                                                      \
    method_table = (MethodBlock**)sysRealloc(method_table,             \
                  (method_table_size + count) * sizeof(MethodBlock*)); \
                                                                       \
    memcpy(&method_table[method_table_size], miranda,                  \
                               count * sizeof(MethodBlock*));          \
    method_table_size += count;                                        \
}

#define fillinMTable(method_table, methods, methods_count)             \
{                                                                      \
    int i;                                                             \
    for(i = 0; i < methods_count; i++, methods++) {                    \
        if((methods->access_flags & (ACC_STATIC | ACC_PRIVATE)) ||     \
               (methods->name[0] == '<'))                              \
            continue;                                                  \
        method_table[methods->method_table_index] = methods;           \
    }                                                                  \
}

void linkClass(Class *class) {
   static MethodBlock *obj_fnlzr_mthd = NULL;

   ClassBlock *cb = CLASS_CB(class);
   Class *super = (cb->access_flags & ACC_INTERFACE) ? NULL : cb->super;

   ITableEntry *spr_imthd_tbl = NULL;
   MethodBlock **method_table = NULL;
   MethodBlock **spr_mthd_tbl = NULL;
   MethodBlock *finalizer;
   MethodBlock *mb;
   FieldBlock *fb;

   int new_methods_count = 0;
   int spr_imthd_tbl_sze = 0;
   int itbl_offset_count = 0;
   int spr_mthd_tbl_sze = 0;
   int method_table_size;
   int new_itable_count;
   int itbl_idx, i, j;
   int spr_flags = 0;

   if(cb->state >= CLASS_LINKED)
       return;

   objectLock(class);

   if(cb->state >= CLASS_LINKED)
       goto unlock;

   if(verbose)
       jam_printf("[Linking class %s]\n", cb->name);

   if(super) {
      ClassBlock *super_cb = CLASS_CB(super);
      if(super_cb->state < CLASS_LINKED)
          linkClass(super);

      spr_flags = super_cb->flags;
      spr_mthd_tbl = super_cb->method_table;
      spr_imthd_tbl = super_cb->imethod_table;
      spr_mthd_tbl_sze = super_cb->method_table_size;
      spr_imthd_tbl_sze = super_cb->imethod_table_size;
   }

   /* Calculate object layout */

   prepareFields(class);

   /* prepare methods */

   for(mb = cb->methods, i = 0; i < cb->methods_count; i++,mb++) {

       /* calculate argument count from signature */

       int count = 0;
       char *sig = mb->type;
       SCAN_SIG(sig, count+=2, count++);

       if(mb->access_flags & ACC_STATIC)
           mb->args_count = count;
       else
           mb->args_count = count + 1;

       mb->class = class;

       /* Set abstract method to stub */
       if(mb->access_flags & ACC_ABSTRACT) {
           mb->code_size = sizeof(abstract_method);
           mb->code = abstract_method;
       }

       if(mb->access_flags & ACC_NATIVE) {

           /* set up native invoker to wrapper to resolve function
              on first invocation */

           mb->native_invoker = &resolveNativeWrapper;

           /* native methods have no code attribute so these aren't filled
              in at load time - as these values are used when creating frame
              set to appropriate values */

           mb->max_locals = mb->args_count;
           mb->max_stack = 0;
       }

#ifdef DIRECT
       else  {
           /* Set the bottom bit of the pointer to indicate the
              method is unprepared */
           mb->code = ((char*)mb->code) + 1;
       }
#endif

       /* Static, private or init methods aren't dynamically invoked, so
         don't stick them in the table to save space */

       if((mb->access_flags & (ACC_STATIC | ACC_PRIVATE)) || (mb->name[0] == '<'))
           continue;

       /* if it's overriding an inherited method, replace in method table */

       for(j = 0; j < spr_mthd_tbl_sze; j++)
           if(mb->name == spr_mthd_tbl[j]->name &&
                        mb->type == spr_mthd_tbl[j]->type &&
                        checkMethodAccess(spr_mthd_tbl[j], class)) {
               mb->method_table_index = spr_mthd_tbl[j]->method_table_index;
               break;
           }

       if(j == spr_mthd_tbl_sze)
           mb->method_table_index = spr_mthd_tbl_sze + new_methods_count++;
   }

   /* construct method table */

   method_table_size = spr_mthd_tbl_sze + new_methods_count;

   if(!(cb->access_flags & ACC_INTERFACE)) {
       method_table = sysMalloc(method_table_size * sizeof(MethodBlock*));

       /* Copy parents method table to the start */
       memcpy(method_table, spr_mthd_tbl, spr_mthd_tbl_sze * sizeof(MethodBlock*));

       /* fill in the additional methods -- we use a
          temporary because fillinMtable alters mb */
       mb = cb->methods;
       fillinMTable(method_table, mb, cb->methods_count);
   }

   /* setup interface method table */

   /* number of interfaces implemented by this class is those implemented by
    * parent, plus number of interfaces directly implemented by this class,
    * and the total number of their superinterfaces */

   new_itable_count = cb->interfaces_count;
   for(i = 0; i < cb->interfaces_count; i++)
       new_itable_count += CLASS_CB(cb->interfaces[i])->imethod_table_size;

   cb->imethod_table_size = spr_imthd_tbl_sze + new_itable_count;
   cb->imethod_table = sysMalloc(cb->imethod_table_size * sizeof(ITableEntry));

   /* copy parent's interface table - the offsets into the method table won't change */

   memcpy(cb->imethod_table, spr_imthd_tbl, spr_imthd_tbl_sze * sizeof(ITableEntry));

   /* now run through the extra interfaces implemented by this class,
    * fill in the interface part, and calculate the number of offsets
    * needed (i.e. the number of methods defined in the interfaces) */

   itbl_idx = spr_imthd_tbl_sze;
   for(i = 0; i < cb->interfaces_count; i++) {
       Class *intf = cb->interfaces[i];
       ClassBlock *intf_cb = CLASS_CB(intf);

       cb->imethod_table[itbl_idx++].interface = intf;
       itbl_offset_count += intf_cb->method_table_size;

       for(j = 0; j < intf_cb->imethod_table_size; j++) {
           Class *spr_intf = intf_cb->imethod_table[j].interface;

           cb->imethod_table[itbl_idx++].interface = spr_intf;
           itbl_offset_count += CLASS_CB(spr_intf)->method_table_size;
       }
   }

   /* if we're an interface all finished - offsets aren't used */

   if(!(cb->access_flags & ACC_INTERFACE)) {
       int *offsets_pntr = sysMalloc(itbl_offset_count * sizeof(int));
       int old_mtbl_size = method_table_size;
       MethodBlock *miranda[MRNDA_CACHE_SZE];
       int miranda_count = 0;
       int mtbl_idx;

       /* run through table again, this time filling in the offsets array -
        * for each new interface, run through it's methods and locate
        * each method in this classes method table */

       for(i = spr_imthd_tbl_sze; i < cb->imethod_table_size; i++) {
           ClassBlock *intf_cb = CLASS_CB(cb->imethod_table[i].interface);
           cb->imethod_table[i].offsets = offsets_pntr;

           for(j = 0; j < intf_cb->methods_count; j++) {
               MethodBlock *intf_mb = &intf_cb->methods[j];

               if((intf_mb->access_flags & (ACC_STATIC | ACC_PRIVATE)) ||
                      (intf_mb->name[0] == '<'))
                   continue;

               /* We scan backwards so that we find methods defined in sub-classes
                  before super-classes.  This ensures we find non-overridden
                  methods before the inherited non-accessible method */
               for(mtbl_idx = method_table_size - 1; mtbl_idx >= 0; mtbl_idx--)
                   if(intf_mb->name == method_table[mtbl_idx]->name &&
                           intf_mb->type == method_table[mtbl_idx]->type) {
                       *offsets_pntr++ = mtbl_idx;
                       break;
                   }

               if(mtbl_idx < 0) {

                   /* didn't find it - add a dummy abstract method (a so-called
                      miranda method).  If it's invoked we'll get an abstract
                      method error */

                   int k;
                   for(k = 0; k < miranda_count; k++)
                       if(intf_mb->name == miranda[k]->name &&
                                   intf_mb->type == miranda[k]->type)
                           break;

                   *offsets_pntr++ = method_table_size + k;

                   if(k == miranda_count) {
                       if(miranda_count == MRNDA_CACHE_SZE) {
                           resizeMTable(method_table, method_table_size, miranda, MRNDA_CACHE_SZE);
                           miranda_count = 0;
                       }
                       miranda[miranda_count++] = intf_mb;
                   }
               }
           }
       }

       if(miranda_count > 0)
           resizeMTable(method_table, method_table_size, miranda, miranda_count);

       if(old_mtbl_size != method_table_size) {
           /* We've created some abstract methods */
           int num_mirandas = method_table_size - old_mtbl_size;

           mb = (MethodBlock *) sysRealloc(cb->methods,
                   (cb->methods_count + num_mirandas) * sizeof(MethodBlock));

           /* If the realloc of the method list gave us a new pointer, the pointers
              to them in the method table are now wrong. */
           if(mb != cb->methods) {
               /*  mb will be left pointing to the end of the methods */
               cb->methods = mb;
               fillinMTable(method_table, mb, cb->methods_count);
           } else
               mb += cb->methods_count;

           cb->methods_count += num_mirandas;

           /* Now we've expanded the method list, replace pointers to
              the interface methods. */

           for(i = old_mtbl_size; i < method_table_size; i++,mb++) {
               memcpy(mb, method_table[i], sizeof(MethodBlock));
               mb->access_flags |= ACC_MIRANDA;
               mb->method_table_index = i;
               mb->class = class;
               method_table[i] = mb;
           }
       }
   }

   cb->method_table = method_table;
   cb->method_table_size = method_table_size;

   /* Handle finalizer */

   /* If this is Object find the finalize method.  All subclasses will
      have it in the same place in the method table.  Note, Object
      should always have a valid finalizer -- but check just in case */

   if(cb->super == NULL) {
       finalizer = findMethod(class, SYMBOL(finalize), SYMBOL(___V));
       if(finalizer && !(finalizer->access_flags & (ACC_STATIC | ACC_PRIVATE))) {
           finalize_mtbl_idx = finalizer->method_table_index;
           obj_fnlzr_mthd = finalizer;
       }
   }

   cb->flags |= spr_flags;

   /* Store the finalizer only if it's overridden Object's.  We don't
      want to finalize every object, and Object's imp is empty */

   if(super && obj_fnlzr_mthd && (finalizer =
               method_table[obj_fnlzr_mthd->method_table_index]) != obj_fnlzr_mthd)
       cb->flags |= FINALIZED;

   /* Handle reference classes */

   if(ref_referent_offset == -1 && cb->name == SYMBOL(java_lang_ref_Reference)) {
       FieldBlock *ref_fb = findField(class, SYMBOL(referent), SYMBOL(sig_java_lang_Object));
       FieldBlock *queue_fb = findField(class, SYMBOL(queue), SYMBOL(sig_java_lang_ref_ReferenceQueue));
       MethodBlock *enqueue_mb = findMethod(class, SYMBOL(enqueue), SYMBOL(___Z));

       if(ref_fb == NULL || queue_fb == NULL || enqueue_mb == NULL) {
           jam_fprintf(stderr, "Expected fields/methods missing in java.lang.ref.Reference\n");
           exitVM(1);
       }

       for(fb = cb->fields, i = 0; i < cb->fields_count; i++,fb++)
           if(fb->u.offset > ref_fb->u.offset)
               fb->u.offset -= sizeof(Object*);

       ref_referent_offset = ref_fb->u.offset = cb->object_size - sizeof(Object*);
       cb->refs_offsets_table[cb->refs_offsets_size-1].end -= sizeof(Object*);

       enqueue_mtbl_idx = enqueue_mb->method_table_index;
       ref_queue_offset = queue_fb->u.offset;

       cb->flags |= REFERENCE;
   }

   if(spr_flags & REFERENCE) {
       if(cb->name == SYMBOL(java_lang_ref_SoftReference))
           cb->flags |= SOFT_REFERENCE;
       else
           if(cb->name == SYMBOL(java_lang_ref_WeakReference))
               cb->flags |= WEAK_REFERENCE;
           else
               if(cb->name == SYMBOL(java_lang_ref_PhantomReference))
                   cb->flags |= PHANTOM_REFERENCE;
   }

   /* Handle class loader classes */

   if(ldr_vmdata_offset == -1 && cb->name == SYMBOL(java_lang_ClassLoader)) {
       FieldBlock *ldr_fb = findField(class, SYMBOL(vmdata), SYMBOL(sig_java_lang_Object));

       if(ldr_fb == NULL) {
           jam_fprintf(stderr, "Expected vmdata field missing in java.lang.ClassLoader\n");
           exitVM(1);
       }

       ldr_vmdata_offset = ldr_fb->u.offset;
       cb->flags |= CLASS_LOADER;
   }

   cb->state = CLASS_LINKED;

unlock:
   objectUnlock(class);
}

Class *initClass(Class *class) {
   ClassBlock *cb = CLASS_CB(class);
   ConstantPool *cp = &cb->constant_pool;
   FieldBlock *fb = cb->fields;
   MethodBlock *mb;
   Object *excep;
   int state, i;

   if(cb->state >= CLASS_INITED)
      return class;

   linkClass(class);
   objectLock(class);

   while(cb->state == CLASS_INITING)
      if(cb->initing_tid == threadSelf()->id) {
         objectUnlock(class);
         return class;
      } else {
          /* FALSE means this wait is non-interruptible.
             An interrupt will appear as if the initialiser
             failed (below), and clearing will lose the
             interrupt status */
          objectWait0(class, 0, 0, FALSE);
      }

   if(cb->state >= CLASS_INITED) {
      objectUnlock(class);
      return class;
   }

   if(cb->state == CLASS_BAD) {
       objectUnlock(class);
       signalException(java_lang_NoClassDefFoundError, cb->name);
       return NULL;
   }

   cb->state = CLASS_INITING;
   cb->initing_tid = threadSelf()->id;

   objectUnlock(class);

   if(!(cb->access_flags & ACC_INTERFACE) && cb->super
              && (CLASS_CB(cb->super)->state != CLASS_INITED)) {
      initClass(cb->super);
      if(exceptionOccurred()) {
          state = CLASS_BAD;
          goto set_state_and_notify;
      }
   }

   /* Never used to bother with this as only static finals use it and
      the constant value's copied at compile time.  However, separate
      compilation can result in a getstatic to a (now) constant field,
      and the VM didn't initialise it... */

   for(i = 0; i < cb->fields_count; i++,fb++)
      if((fb->access_flags & ACC_STATIC) && fb->constant) {
         if((*fb->type == 'J') || (*fb->type == 'D'))
            fb->u.static_value.l = *(u8*)&(CP_INFO(cp, fb->constant));
         else
            fb->u.static_value.u = resolveSingleConstant(class, fb->constant);
      }

   if((mb = findMethod(class, SYMBOL(class_init), SYMBOL(___V))) != NULL)
      executeStaticMethod(class, mb);

   if((excep = exceptionOccurred())) {
       Class *error, *eiie;

       clearException();

       /* Don't wrap exceptions of type java.lang.Error... */
       if((error = findSystemClass0(SYMBOL(java_lang_Error)))
                 && !isInstanceOf(error, excep->class)
                 && (eiie = findSystemClass(SYMBOL(java_lang_ExceptionInInitializerError)))
                 && (mb = findMethod(eiie, SYMBOL(object_init), SYMBOL(_java_lang_Throwable__V)))) {

           Object *ob = allocObject(eiie);

           if(ob != NULL) {
               executeMethod(ob, mb, excep);
               setException(ob);
           }
       } else
           setException(excep);

       state = CLASS_BAD;
   } else
       state = CLASS_INITED;

set_state_and_notify:
   objectLock(class);
   cb->state = state;

   objectNotifyAll(class);
   objectUnlock(class);

   return state == CLASS_BAD ? NULL : class;
}

char *findFileEntry(char *path, int *file_len) {
    int read_len;
    char *data;
    FILE *fd;

    if((fd = fopen(path, "r")) == NULL)
        return NULL;

    fseek(fd, 0L, SEEK_END);
    *file_len = ftell(fd);
    fseek(fd, 0L, SEEK_SET);

    data = sysMalloc(*file_len);
    read_len = fread(data, sizeof(char), *file_len, fd);
    fclose(fd);

    if(read_len == *file_len)
        return data;

    sysFree(data);
    return NULL;
}

void defineBootPackage(char *classname, int index) {
    char *last_slash = strrchr(classname, '/');

    if(last_slash != NULL && last_slash != classname) {
        int len = last_slash - classname + 1;
        PackageEntry *package = sysMalloc(sizeof(PackageEntry) + len);
        PackageEntry *hashed;

        package->index = index;
        slash2dots2buff(classname, package->name, len);

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(((PackageEntry*)ptr)->name)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2 && \
            utf8Comp(((PackageEntry*)ptr1)->name, ((PackageEntry*)ptr2)->name))

        /* Add if absent, no scavenge, locked */
        findHashEntry(boot_packages, package, hashed, TRUE, FALSE, TRUE);

        if(package != hashed)
            sysFree(package);
    }
}

Class *loadSystemClass(char *classname) {
	int file_len, fname_len = strlen(classname) + 8;
    char buff[max_cp_element_len + fname_len];
    char filename[fname_len];
    Class *class = NULL;
    char *data = NULL;
    int i;

    filename[0] = '/';
    strcat(strcpy(&filename[1], classname), ".class");

    for(i = 0; i < bcp_entries && data == NULL; i++)
        if(bootclasspath[i].zip)
            data = findArchiveEntry(filename + 1, bootclasspath[i].zip,
                                    &file_len);
        else
            data = findFileEntry(strcat(strcpy(buff, bootclasspath[i].path),
                                 filename), &file_len);

    if(data == NULL) {
        signalException(java_lang_NoClassDefFoundError, classname);
        return NULL;
    }

    defineBootPackage(classname, i - 1);

    class = defineClass(classname, data, 0, file_len, NULL);
    sysFree(data);

    if(verbose && class)
        jam_printf("[Loaded %s from %s]\n", classname, bootclasspath[i-1].path);

    return class;
}

void addInitiatingLoaderToClass(Object *class_loader, Class *class) {
    ClassBlock *cb = CLASS_CB(class);

    /* The defining class loader is automatically an initiating
       loader so don't add again */
    if(cb->class_loader != class_loader)
        addClassToHash(class, class_loader);
}

//NVM method to initialize the classes hash table
void initializeClassesHT(Object *class_loader){

#undef COMPARE

#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2)

	int count_hash_entries = 0; //Testing variable
	int count_name_entries = 0; //Testing variable

	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);
	HashTable *table;
	if(vmdata == NULL){
		vmdata = allocObject(ldr_new_unloader->class);
		table = calloc(1, sizeof(HashTable));
		initHashTable((*table), CLASS_INITSZE, TRUE);
	}
	else{
		INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset) = table;
	}
	if(access (FILENAME_CLASSES_HT, F_OK) != -1) {
		pointer_file_exists_classes = TRUE;
	}
	if(access (FILENAME_CLASSES_NAMES, F_OK) != -1) {
		name_file_exists_classes = TRUE;
	}
	if((pointer_file_exists_classes == TRUE) && (name_file_exists_classes == TRUE)) {
		hash_file_classes = fopen (FILENAME_CLASSES_HT, "r+b");
		name_file_classes = fopen (FILENAME_CLASSES_NAMES, "r+b");
		if((hash_file_classes == NULL) || (name_file_classes == NULL)) {
			printf("ERROR: error trying to open classes hash table context files\n");
			exit(EXIT_FAILURE);
		}
		size_t len_pointer = 0;
		size_t len_string = 0;
		Class *interned = NULL;
		Class *to_add = (Class*)calloc(1, sizeof(Class*));
		len_pointer = fread(&to_add, sizeof(Class*), 1, hash_file_classes);
		while (len_pointer == 1) {
			do {

				unsigned short string_length = 0;
				len_string = fread(&string_length, sizeof(unsigned short), 1, name_file_classes);

				char *class_name = (char*)calloc(1, string_length * sizeof(char));
				int count = fread(class_name, sizeof(char), string_length, name_file_classes);

				if(count == string_length){
					count_name_entries++;
				}
				else{
					printf("ERROR: error trying to read class name from classname file\n");
					exit(EXIT_FAILURE);
				}

				//addHashEntry() adapted
				int hash = utf8Hash(class_name);
				int i;

				Thread *self;
				self = threadSelf();
				lockHashTable0(&(*table), self);

				i = hash & ((*table).hash_size - 1);

				for(;;) {
					interned = (*table).hash_table[i].data;
					if((interned == NULL) || (COMPARE(to_add, interned, hash, (*table).hash_table[i].hash)))
						break;

					i = (i+1) & ((*table).hash_size - 1);
				}
				(*table).hash_table[i].hash = hash;
				interned = (*table).hash_table[i].data = PREPARE(to_add);
				(*table).hash_count++;

				if(((*table).hash_count * 4) > ((*table).hash_size * 3)) {
					resizeHash(&(*table), (*table).hash_size * 2);
				}

				unlockHashTable0(&(*table), self);
				//End of addHashEntry()

				interned = NULL;

				//findOnlyHashEntry() adapted
				hash = utf8Hash(class_name);

				self = threadSelf();
				lockHashTable0(&(*table), self);

				i = hash & ((*table).hash_size - 1);

				for(;;) {
					interned = (*table).hash_table[i].data;
					if((interned == NULL) || (COMPARE(to_add, interned, hash, (*table).hash_table[i].hash)))
						break;

					i = (i+1) & ((*table).hash_size - 1);
				}

				unlockHashTable0(&(*table), self);
				//End of finfOnlyHashEntry()

			}while(interned == NULL);
			count_hash_entries++;
			len_pointer = fread(&to_add, sizeof(Class*), 1, hash_file_classes);
		}
		fclose(hash_file_classes);
		fclose(name_file_classes);
	}
	//UNIT TESTS
	if(testing_mode_classes) {
		log_test_results("initaliseClasses_fileExists", pointer_file_exists_classes && name_file_exists_classes);
		if(count_hash_entries == table->hash_count){
			log_test_results("initaliseClasses_recoverClassesPOINTERS", TRUE);
		} else {
			log_test_results("initaliseClasses_recoverClassesPOINTERS", FALSE);
		}
		if(count_name_entries == count_hash_entries){
			log_test_results("initaliseClasses_recoverClassesNAMES", TRUE);
		} else {
			log_test_results("initaliseClasses_recoverClassesNAMES", FALSE);
		}
	}

}


Class *findHashedClass(char *classname, Object *class_loader) {
    HashTable *table;
    Class *class;
    char *name;

    /* If the class name is not in the utf8 table it can't
       have been loaded */

    if((name = findUtf8(classname)) == NULL) {

        return NULL;
    }

    if(class_loader == NULL) {
        table = &boot_classes;
    }
    else {
    	if(is_persistent_classes){
    		if(classes_ht_initialized == FALSE){
    			initializeClassesHT(class_loader);
    			classes_ht_initialized = TRUE;
    		}
    	}
    	Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);



        if(vmdata == NULL) {
            return NULL;
        }

        table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
    }

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(ptr)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2) && \
            (ptr1 == CLASS_CB((Class *)ptr2)->name)


    /* Do not add if absent, no scavenge, locked */
    findHashEntry((*table), name, class, FALSE, FALSE, TRUE);

   return class;
}

Class *findSystemClass0(char *classname) {
	Class *class = findHashedClass(classname, NULL);

   if(class == NULL) {

	   class = loadSystemClass(classname);

   }
   if(!exceptionOccurred()) {
      linkClass(class);
   }

   return class;
}

Class *findSystemClass(char *classname) {
   Class *class = findSystemClass0(classname);

   if(!exceptionOccurred())
       initClass(class);

   return class;
}

Class *findArrayClassFromClassLoader(char *classname, Object *class_loader) {
   Class *class = findHashedClass(classname, class_loader);

   if(class == NULL) {
       if((class = createArrayClass(classname, class_loader)) != NULL)
           addInitiatingLoaderToClass(class_loader, class);
   }
   return class;
}

Class *findPrimitiveClass(char prim_type) {
   int index;
   Class *prim;
   char *classname;

   switch(prim_type) {
      case 'Z':
          classname = SYMBOL(boolean);
          index = PRIM_IDX_BOOLEAN;
          break;
      case 'B':
          classname = SYMBOL(byte);
          index = PRIM_IDX_BYTE;
          break;
      case 'C':
          classname = SYMBOL(char);
          index = PRIM_IDX_CHAR;
          break;
      case 'S':
          classname = SYMBOL(short);
           index = PRIM_IDX_SHORT;
          break;
      case 'I':
          classname = SYMBOL(int);
           index = PRIM_IDX_INT;
          break;
      case 'F':
          classname = SYMBOL(float);
           index = PRIM_IDX_FLOAT;
          break;
      case 'J':
          classname = SYMBOL(long);
           index = PRIM_IDX_LONG;
          break;
      case 'D':
          classname = SYMBOL(double);
           index = PRIM_IDX_DOUBLE;
          break;
      case 'V':
          classname = SYMBOL(void);
           index = PRIM_IDX_VOID;
          break;
      default:
          signalException(java_lang_NoClassDefFoundError, NULL);
          return NULL;
          break;
   }

   prim = prim_classes[index];
   return prim ? prim : createPrimClass(classname, index);
}

Class *findNonArrayClassFromClassLoader(char *classname, Object *loader) {
	Class *class = findHashedClass(classname, loader);

    if(class == NULL) {
        char *dot_name = slash2dots(classname);
        Object *string = createString(dot_name);
        Object *excep;

        sysFree(dot_name);
        if(string == NULL)
            return NULL;

        if(loadClass_mtbl_idx == -1) {
            MethodBlock *mb = lookupMethod(loader->class, SYMBOL(loadClass),
                            SYMBOL(_java_lang_String__java_lang_Class));
            if(mb == NULL)
                return NULL;

            loadClass_mtbl_idx = mb->method_table_index;
        }

        /* The public loadClass is not synchronized.
           Lock the class-loader to be thread-safe */
        objectLock(loader);
        class = *(Class**)executeMethod(loader,
                    CLASS_CB(loader->class)->method_table[loadClass_mtbl_idx], string);
        objectUnlock(loader);

        if((excep = exceptionOccurred()) || class == NULL) {
            clearException();
            signalChainedException(java_lang_NoClassDefFoundError, classname, excep);
            return NULL;
        }

        addInitiatingLoaderToClass(loader, class);

        if(verbose && (CLASS_CB(class)->class_loader == loader))
            jam_printf("[Loaded %s]\n", classname);
    }
    return class;
}

Class* findInFile (char* name_to_find){
	Class *class;
	FILE *file = fopen (NH_FILENAME, "rt");
	char *line = NULL;
	size_t len = 0;
	size_t read = 0;
	char *name_to_add;
	int pointer;
	int i;

	while ((read = getline(&line, &len, file)) != -1)	{
		char *name_to_add;
		name_to_add = calloc(1 ,read*sizeof(char));
		strcpy(name_to_add, line);
		name_to_add[strlen(name_to_add)-1] = 0;

		if (name_to_add[1] == 'x')
			continue;

		if (!strcmp(name_to_find,name_to_add)){
			for (i = 0; i < 3; i++){
				if ((read = getline(&line, &len, file)) != -1){
					name_to_add = calloc(1, read*sizeof(char));
					strcpy(name_to_add, line);
					name_to_add[strlen(name_to_add)-1] = 0;
					pointer = (int) strtoll (name_to_add,NULL,16);

					if (i==0)
						class = (Class*) pointer;
					if (i==1)
						class->lock = (unsigned int) pointer;
					if (i==2)
						class->class = (Class *) pointer;
				}
			}
				fclose(file);
				return class;
		}
	}
	fclose(file);
	return NULL;
}

Class *findClassFromClassLoader_persistent(char *classname, Object *loader) {
	Class *class;
	if(*classname == '[')
		return findArrayClassFromClassLoader(classname, loader);

	if(loader != NULL){
		if(access (NH_FILENAME, F_OK) != -1){
			class = findInFile(classname);
			if (class != NULL){
				return class;
			}
		}

		class = findNonArrayClassFromClassLoader(classname, loader);
		FILE * nonHashedFile;
		nonHashedFile = fopen (NH_FILENAME, "a+");

		fprintf(nonHashedFile, "%s%s", classname, SEP);
		fprintf(nonHashedFile, "%p%s", class, SEP);
		fprintf(nonHashedFile, "%u%s", class->lock, SEP);
		fprintf(nonHashedFile, "%p%s", class->class, SEP);

		fclose(nonHashedFile);
		return class;
	}

	return findSystemClass0(classname);
}

Class *findClassFromClassLoader(char *classname, Object *loader) {
	//XXX uncomment for different approach
	/*if (is_persistent_classes)
		return findClassFromClassLoader_persistent(classname, loader);
	else{*/

		if(*classname == '[')
			return findArrayClassFromClassLoader(classname, loader);

		if(loader != NULL)
			return findNonArrayClassFromClassLoader(classname, loader);

		return findSystemClass0(classname);
	//}
}

Object *getSystemClassLoader() {
    Class *class_loader = findSystemClass(SYMBOL(java_lang_ClassLoader));

    if(!exceptionOccurred()) {
        MethodBlock *mb;

        if((mb = findMethod(class_loader, SYMBOL(getSystemClassLoader),
                                          SYMBOL(___java_lang_ClassLoader))) != NULL) {
            Object *system_loader = *(Object**)executeStaticMethod(class_loader, mb);

            if(!exceptionOccurred())
                return system_loader;
        }
    }
    return NULL;
}

Object *createBootPackage(PackageEntry *package_entry) {
    Object *name = createString(package_entry->name);

    if(name != NULL) {
        Object *package = *(Object**)executeStaticMethod(
                                            vm_loader_create_package->class,
                                            vm_loader_create_package, name,
                                            package_entry->index);

        if(!exceptionOccurred())
            return package;
    }

    return NULL;
}

Object *bootPackage(char *package_name) {
    PackageEntry *hashed;

#undef HASH
#undef COMPARE
#define HASH(ptr) utf8Hash(ptr)
#define COMPARE(ptr1, ptr2, hash1, hash2) (hash1 == hash2 && \
                                 utf8Comp(ptr1, ((PackageEntry*)ptr2)->name))

    /* Do not add if absent, no scavenge, locked */
    findHashEntry(boot_packages, package_name, hashed, FALSE, FALSE, TRUE);

    if(hashed != NULL)
        return createBootPackage(hashed);

    return NULL;
}

#define ITERATE(ptr)                                       \
    if((data[--count] = createBootPackage(ptr)) == NULL) { \
        array = NULL;                                      \
        goto error;                                        \
    }

Object *bootPackages() {
    Object **data, *array;
    int count;

    lockHashTable(boot_packages);

    count = hashTableCount(boot_packages);
    if((array = allocArray(package_array_class, count, sizeof(Object*))) == NULL)
        goto error;

    data = ARRAY_DATA(array, Object*);
    hashIterate(boot_packages);

error:
    unlockHashTable(boot_packages);
    return array;
}

/* gc support for marking classes */

#undef ITERATE
#define ITERATE(ptr) markRoot(ptr)

void markBootClasses() {
   int i;

   hashIterate(boot_classes);

   for(i = 0; i < MAX_PRIM_CLASSES; i++)
       if(prim_classes[i] != NULL)
           markRoot((Object*)prim_classes[i]);
}

#undef ITERATE
#define ITERATE(ptr)  threadReference((Object**)ptr)

void threadBootClasses() {
   int i;

   hashIterateP(boot_classes);

   for(i = 0; i < MAX_PRIM_CLASSES; i++)
       if(prim_classes[i] != NULL)
           threadReference((Object**)&prim_classes[i]);
}

#undef ITERATE
#define ITERATE(ptr)                                         \
    if(CLASS_CB((Class *)ptr)->class_loader == class_loader) \
        markObject(ptr, mark)

void markLoaderClasses(Object *class_loader, int mark) {
    Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

    if(vmdata != NULL) {
        HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
        hashIterate((*table));
    }
}

#undef ITERATE
#define ITERATE(ptr) threadReference((Object**)ptr)

void threadLoaderClasses(Object *class_loader) {
    Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

    if(vmdata != NULL) {
        HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
        hashIterateP((*table));
    }
}

void freeClassData(Class *class) {
    ClassBlock *cb = CLASS_CB(class);
    int i;

    if(IS_ARRAY(cb)) {
        gcPendingFree(cb->interfaces);
        return;
    }

    gcPendingFree((void*)cb->constant_pool.type);
    gcPendingFree(cb->constant_pool.info);
    gcPendingFree(cb->interfaces);

    for(i = 0; i < cb->fields_count; i++) {
        FieldBlock *fb = &cb->fields[i];

        if(fb->annotations != NULL) {
            gcPendingFree(fb->annotations->data);
            gcPendingFree(fb->annotations);
        }
    }

    gcPendingFree(cb->fields);

    for(i = 0; i < cb->methods_count; i++) {
        MethodBlock *mb = &cb->methods[i];

#ifdef DIRECT
        if(!((uintptr_t)mb->code & 0x3)) {
#ifdef INLINING
            if(cb->state >= CLASS_LINKED)
                freeMethodInlinedInfo(mb);
#endif
            gcPendingFree(mb->code);
        } else
            if(!(mb->access_flags & ACC_ABSTRACT))
                gcPendingFree((void*)((uintptr_t)mb->code & ~3));
#else
        if(!(mb->access_flags & ACC_ABSTRACT))
            gcPendingFree(mb->code);
#endif

        gcPendingFree(mb->exception_table);
        gcPendingFree(mb->line_no_table);
        gcPendingFree(mb->throw_table);

        if(mb->annotations != NULL) {
            if(mb->annotations->annotations != NULL) {
                gcPendingFree(mb->annotations->annotations->data);
                gcPendingFree(mb->annotations->annotations);
            }
            if(mb->annotations->parameters != NULL) {
                gcPendingFree(mb->annotations->parameters->data);
                gcPendingFree(mb->annotations->parameters);
            }
            if(mb->annotations->dft_val != NULL) {
                gcPendingFree(mb->annotations->dft_val->data);
                gcPendingFree(mb->annotations->dft_val);
            }
            gcPendingFree(mb->annotations);
        }
    }

    gcPendingFree(cb->methods);
    gcPendingFree(cb->inner_classes);

    if(cb->annotations != NULL) {
        gcPendingFree(cb->annotations->data);
        gcPendingFree(cb->annotations);
    }

   if(cb->state >= CLASS_LINKED) {
        ClassBlock *super_cb = CLASS_CB(cb->super);

        /* interfaces do not have a method table, or
            imethod table offsets */
        if(!IS_INTERFACE(cb)) {
             int spr_imthd_sze = super_cb->imethod_table_size;

            gcPendingFree(cb->method_table);
            if(cb->imethod_table_size > spr_imthd_sze)
                gcPendingFree(cb->imethod_table[spr_imthd_sze].offsets);
        }

        gcPendingFree(cb->imethod_table);

        if(cb->refs_offsets_table != super_cb->refs_offsets_table)
            gcPendingFree(cb->refs_offsets_table);
    }
}

void freeClassLoaderData(Object *class_loader) {
    Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

    if(vmdata != NULL) {
        HashTable *table = INST_DATA(vmdata, HashTable*, ldr_data_tbl_offset);
        gcFreeHashTable((*table));
        gcPendingFree(table);
    }
}

/* Add a library unloader object to the class loader for the
   library contained within entry.  The library has an unload
   function, which will be called from the unloader finalizer
   when the class loader is garbage collected */
void newLibraryUnloader(Object *class_loader, void *entry) {
    Object *vmdata = INST_DATA(class_loader, Object*, ldr_vmdata_offset);

    if(vmdata != NULL)
        executeMethod(vmdata, ldr_new_unloader, (long long)(uintptr_t)entry);
}

int parseBootClassPath(char *cp_var) {
    char *cp, *pntr, *start;
    int i, j, len, max = 0;
    struct stat info;

    cp = sysMalloc(strlen(cp_var)+1);
    strcpy(cp, cp_var);

    for(i = 0, start = pntr = cp; *pntr; pntr++) {
        if(*pntr == ':') {
            if(start != pntr) {
                *pntr = '\0';
                i++;
            }
            start = pntr+1;
        }
    }
    if(start != pntr)
        i++;

    bootclasspath = sysMalloc(sizeof(BCPEntry)*i);

    for(j = 0, pntr = cp; i > 0; i--) {
        while(*pntr == ':')
            pntr++;

        start = pntr;
        pntr += (len = strlen(pntr))+1;

        if(stat(start, &info) == 0) {
            if(S_ISDIR(info.st_mode)) {
                bootclasspath[j].zip = NULL;
                if(len > max)
                    max = len;
            } else
                if((bootclasspath[j].zip = processArchive(start)) == NULL)
                    continue;
            bootclasspath[j++].path = start;
        }
    }

    max_cp_element_len = max;

    return bcp_entries = j;
}

void setClassPath(char *cmdlne_cp) {
    char *env;
    classpath = cmdlne_cp ? cmdlne_cp :
                 ((env = getenv("CLASSPATH")) ? env : ".");
}

char *getClassPath() {
    return classpath;
}

#ifdef __linux__
int filter(const struct dirent *entry) {
#else
int filter(struct dirent *entry) {
#endif
    int len = strlen(entry->d_name);
    char *ext = (char*)&entry->d_name[len-4];

    return len >= 4 && (strcasecmp(ext, ".zip") == 0 ||
                        strcasecmp(ext, ".jar") == 0);
}

void scanDirForJars(char *dir) {
    int bootpathlen = strlen(bootpath) + 1;
    int dirlen = strlen(dir);
    struct dirent **namelist;
    int n;

    n = scandir(dir, &namelist, &filter, &alphasort);

    if(n >= 0) {
        while(--n >= 0) {
            char *buff;
            bootpathlen += strlen(namelist[n]->d_name) + dirlen + 2;
            buff = sysMalloc(bootpathlen);

            strcat(strcat(strcat(strcat(strcpy(buff, dir), "/"),
                                 namelist[n]->d_name), ":"), bootpath);

            sysFree(bootpath);
            bootpath = buff;
            free(namelist[n]);
        }
        free(namelist);
    }
}

void scanDirsForJars(char *directories) {
    int dirslen = strlen(directories);
    char *pntr, *end, *dirs = sysMalloc(dirslen + 1);

    strcpy(dirs, directories);
    for(end = pntr = &dirs[dirslen]; pntr != dirs; pntr--) {
        if(*pntr == ':') {
            char *start = pntr + 1;
            if(start != end)
                scanDirForJars(start);

            *(end = pntr) = '\0';
        }
    }

    if(end != dirs)
        scanDirForJars(dirs);

    sysFree(dirs);
}

char *setBootClassPath(char *cmdlne_bcp, char bootpathopt) {
    char *endorsed_dirs;

    if(cmdlne_bcp)
        switch(bootpathopt) {
            case 'a':
            case 'p':
                bootpath = sysMalloc(strlen(DFLT_BCP) + strlen(cmdlne_bcp) + 2);
                if(bootpathopt == 'a')
                    strcat(strcat(strcpy(bootpath, DFLT_BCP), ":"), cmdlne_bcp);
                else
                    strcat(strcat(strcpy(bootpath, cmdlne_bcp), ":"), DFLT_BCP);
                break;

            case 'c':
                bootpath = sysMalloc(strlen(JAMVM_CLASSES) + strlen(cmdlne_bcp) + 2);
                strcat(strcat(strcpy(bootpath, JAMVM_CLASSES), ":"), cmdlne_bcp);
                break;

            case 'v':
                bootpath = sysMalloc(strlen(CLASSPATH_CLASSES) + strlen(cmdlne_bcp) + 2);
                strcat(strcat(strcpy(bootpath, cmdlne_bcp), ":"), CLASSPATH_CLASSES);
                break;

            default:
                bootpath = sysMalloc(strlen(cmdlne_bcp) + 1);
                strcpy(bootpath, cmdlne_bcp);
        }
    else {
        char *env = getenv("BOOTCLASSPATH");
        char *path = env ? env : DFLT_BCP;
        bootpath = sysMalloc(strlen(path) + 1);
        strcpy(bootpath, path);
    }

    endorsed_dirs = getCommandLineProperty("java.endorsed.dirs");
    if(endorsed_dirs == NULL)
        endorsed_dirs = INSTALL_DIR"/share/jamvm/endorsed";

    scanDirsForJars(endorsed_dirs);

    return bootpath;
}

char *getBootClassPath() {
    return bootpath;
}

int bootClassPathSize() {
    return bcp_entries;
}

Object *bootClassPathResource(char *filename, int index) {
    Object *res = NULL;

    if(index < bcp_entries) {
        char *buff, *path = bootclasspath[index].path;
        int path_len = strlen(path);

        if(path[0] != '/') {
            char *cwd = getCwd();
            path_len += strlen(cwd) + 1;
            path = strcat(strcat(strcpy(sysMalloc(path_len + 1), cwd), "/"), path);
        }

        /* Alloc enough space for Jar file URL -- jar:file://<path>!/<filename> */
        buff = sysMalloc(strlen(filename) + path_len + 14);

        if(bootclasspath[index].zip != NULL) {
            while(*filename == '/')
                filename++;

            if(findArchiveDirEntry(filename, bootclasspath[index].zip) == NULL)
                goto out;

            sprintf(buff, "jar:file://%s!/%s", path, filename);
        } else {
            struct stat info;

            sprintf(buff, "file://%s/%s", path, filename);
            if(stat(&buff[7], &info) != 0 || S_ISDIR(info.st_mode))
                goto out;
        }

        res = createString(buff);

out:
        if(path != bootclasspath[index].path)
            sysFree(path);
        sysFree(buff);
    }

    return res;
}


void initialiseClass(InitArgs *args) {
    char *bcp = setBootClassPath(args->bootpath, args->bootpathopt);
    FieldBlock *hashtable = NULL;
    Class *loader_data_class;
    Class *vm_loader_class;

    if(args->persistent_heap == TRUE){
    		is_persistent_classes = TRUE;
    		testing_mode_classes = TRUE;
    }

    if(!(bcp && parseBootClassPath(bcp))) {
        jam_fprintf(stderr, "bootclasspath is empty!\n");
        exitVM(1);
    }

    verbose = args->verboseclass;
    setClassPath(args->classpath);

    /* Init hash table, and create lock */
    initHashTable(boot_classes, CLASS_INITSZE, TRUE);

    initHashTable(boot_packages, PCKG_INITSZE, TRUE);

    loader_data_class = findSystemClass0(SYMBOL(jamvm_java_lang_VMClassLoaderData));
    if(loader_data_class != NULL) {
        ldr_new_unloader = findMethod(loader_data_class, SYMBOL(newLibraryUnloader),
                                                         SYMBOL(_J__V));
        hashtable = findField(loader_data_class, SYMBOL(hashtable), SYMBOL(J));

    }


    if(hashtable == NULL || ldr_new_unloader == NULL) {
        jam_fprintf(stderr, "Fatal error: Bad VMClassLoaderData (missing or corrupt)\n");
        exitVM(1);
    }
    ldr_data_tbl_offset = hashtable->u.offset;


    vm_loader_class = findSystemClass0(SYMBOL(java_lang_VMClassLoader));
    if(vm_loader_class != NULL)
       vm_loader_create_package =
                  findMethod(vm_loader_class, SYMBOL(createBootPackage),
                             SYMBOL(_java_lang_String_I__java_lang_Package));

    if(vm_loader_create_package == NULL) {
        jam_fprintf(stderr, "Fatal error: Bad java.lang.VMClassLoader (missing or corrupt)\n");
        exitVM(1);
    }

    package_array_class = findArrayClass(SYMBOL(array_java_lang_Package));
    registerStaticClassRef(&package_array_class);

    if(package_array_class == NULL) {
        jam_fprintf(stderr, "Fatal error: missing java.lang.Package\n");
        exitVM(1);
    }

    /* Register the address of where the java.lang.Class ref _will_ be */
    registerStaticClassRef(&java_lang_Class);
}
