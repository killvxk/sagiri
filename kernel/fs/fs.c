#include<fs/fs.h>
#include<lib/stdio.h>
#include<driver/ide.h>
#include<sync/spinlock.h>
#include<lib/string.h>
#include<debug/debug.h>
#include<mm/malloc.h>
#include<proc/cpu.h>
#include<dev/console.h>
struct superblock sb;
struct file ftable[512];
struct filedescriptor fdtable[512];
struct spinlock fslock;
struct file* rootfile;
uint64_t* bitmappage;
struct superblock readsuperblock(){
    char buf[512];
    readsect((uint64_t)buf, 0);
    struct superblock sb = *((struct superblock *)buf);
    return sb;
}
uint64_t bitmapvarifyused(uint64_t no){
    uint64_t pageno = (no / 8) / 512;
    uint64_t pageoff = (no / 8) % 512;
    uint8_t* c = (uint8_t*)(bitmappage[pageno]);
    if(c[pageoff] & (1u << (no % 8))){
        return 1;
    }else{
        return 0;
    }
}
uint64_t bitmapgetfreeblock(uint64_t from, uint64_t to){
    for(uint64_t i = from; i < to; i++){
        if(!bitmapvarifyused(i)){
            return i;
        }
    }
    return -1;
}
void bitmapsetused(uint64_t no){
    uint64_t pageno = (no / 8) / 512;
    uint64_t pageoff = (no / 8) % 512;
    uint8_t* c = (uint8_t*)(bitmappage[pageno]);
    c[pageoff] |= (1u << (no % 8));
    writesect(bitmappage[pageno], 1 + pageno);
}
void bitmapsetunused(uint64_t no){
    uint64_t pageno = (no / 8) / 512;
    uint64_t pageoff = (no / 8) % 512;
    uint8_t* c = (uint8_t*)(bitmappage[pageno]);
    c[pageoff] &= (~(1u << (no % 8)));
    writesect(bitmappage[pageno], 1 + pageno);
}
struct inode getinode(uint64_t inodenum){
    uint64_t no = inodenum / 8 + 1 + sb.bitmapblock;
    char buf[512];
    readsect((uint64_t)buf, no);
    struct inode ret = *(((struct inode*)buf) + inodenum % 8);
    return ret;
}
void setinode(uint64_t inodenum, struct inode* node){
    uint64_t no = inodenum / 8 + 1 + sb.bitmapblock;
    char buf[512];
    readsect((uint64_t)buf, no);
    *(((struct inode*)buf) + inodenum % 8) = *node;
    writesect((uint64_t)buf, no);
}
int64_t allocinode(enum filetype type){
    int64_t freeblocknum = bitmapgetfreeblock(1 + sb.bitmapblock, 1 + sb.bitmapblock + sb.inodeblocks);
    if(freeblocknum < 0){
        return -1;
    }
    char buf[512];
    readsect((uint64_t)buf, freeblocknum);
    struct inode* node = (struct inode*)buf;
    uint64_t off = -1;
    for(uint64_t i = 0; i < 8; i++){
        if(node[i].type == file_unused){
            off = i;
            break;
        }
    }
    assert(node[off].type == file_unused);
    node[off].type = type;
    node[off].size = 0;
    uint64_t hasfree = 0;
    for(uint64_t i = 0; i < 8; i++){
        if(node[i].type == file_unused){
            hasfree = 1;
            break;
        }
    }
    if(!hasfree){
        bitmapsetused(freeblocknum);
    }
    writesect((uint64_t)buf, freeblocknum);
    return (freeblocknum - 1 - sb.bitmapblock) * 8 + off;
}
void freeinode(uint64_t no){
    struct inode node = getinode(no);
    node.type = file_unused;
    setinode(no, &node);
    bitmapsetunused(1 + sb.bitmapblock + no / 8);
}
void getblock(uint64_t pagenum, char* buf){
    uint64_t no = pagenum + 1 + sb.bitmapblock + sb.inodeblocks;
    readsect((uint64_t)buf, no);
}
void setblock(uint64_t pagenum, char* buf){
    uint64_t no = pagenum + 1 + sb.bitmapblock + sb.inodeblocks;
    writesect((uint64_t)buf, no);
}
int64_t allocblock(){
    int64_t no = bitmapgetfreeblock(1 + sb.bitmapblock + sb.inodeblocks, 1 + sb.bitmapblock + sb.inodeblocks + sb.datablocks);
    if(no < 0){
        return -1;
    }
    bitmapsetused(no);
    return no - 1 - sb.bitmapblock - sb.inodeblocks;
}
void freeblock(uint64_t no){
    bitmapsetunused(1 + sb.bitmapblock + sb.inodeblocks + no);
}
struct file* allocfile(){
    for(uint64_t i = 0; i < 512; i++){
        if(ftable[i].inode.type == file_unused){
            ftable[i].inode.type = file_file;
            ftable[i].parent = 0;
            return &(ftable[i]);
        }
    }
    return 0;
}
void freefile(struct file* f){
    f->inode.type = file_unused;
}
struct filedescriptor* allocfiledescriptor(){
    for(uint64_t i = 0; i < 512; i++){
        if(fdtable[i].used == 0){
            fdtable[i].used = 1;
            return &(fdtable[i]);
        }
    }
    return 0;
}
void freefiledescriptor(struct filedescriptor* fd){
    fd->used = 0;
}
struct file* getfileptrfromnum(uint64_t inodenum){
    for(uint64_t i = 0; i < 512; i++){
        if(ftable[i].inode.type != file_unused && ftable[i].inodenum == inodenum){
            return &(ftable[i]);
        }
    }
    return 0;
}
void printfile(uint64_t nodenum){
    struct inode n = getinode(nodenum);
    if(n.type == file_directory){
        uint64_t childnum = n.size / 16;
        printf("directory, inode = %d, childnum = %d\n", nodenum, childnum);
        uint64_t blockno = (childnum + 31) / 32;
        uint64_t currentchild = 0;
        for(uint64_t i = 0; i < blockno; i++){
            struct dirent ds[32];
            getblock(n.addr[i], (char*)ds);
            for(uint64_t c = 0; c < 32; c++){
                printf("    child, inode = %d, name = %s\n", ds[c].inodenum, ds[c].name);
                printfile(ds[c].inodenum);
                currentchild++;
                if(currentchild >= childnum){
                    break;
                }
            }
        }
    }else{
        printf("file, inode = %d, size = %d\n", nodenum, n.size);
    }
}
uint64_t verifyfilename(char* name){
    uint64_t length = 0;
    for(uint64_t i = 0; i < 100; i++){
        if(name[i] == 0){
            length = i;
            break;
        }
    }
    if(length == 100 || length < 2){
        return 0;
    }
    if(name[0] == '/' && name[length - 1] != '/'){
        return 1;
    }else{
        return 0;
    }
}
int64_t finditemindirectory(struct file* parent, char* name){
    assert(strlen(name) <= 12);
    uint64_t childnum = parent->inode.size / 16;
    uint64_t childptr = 0;
    for(uint64_t i = 0; i < 14; i++){
        uint32_t blockaddr = parent->inode.addr[i];
        char block[512];
        getblock(blockaddr, block);
        for(uint64_t j = 0; j < 32; j++){
            struct dirent* d = (struct dirent*)block + j;
            if(strncmp(d->name, name, 12) == 0){
                return d->inodenum;
            }
            childptr++;
            if(childptr >= childnum){
                return -1;
            }
        }
    }
    return -1;
}
struct file* getparentinode(char* name){
    uint64_t start = 1;
    uint64_t end = 1;
    uint64_t length = strlen(name);
    struct file* cur = rootfile;
    cur->ref += 1;
    assert(cur->inode.type == file_directory);
    while(end < length){
        if(name[end] == '/'){
            char namebuf[20];
            for(uint64_t i = start; i < end; i++){
                namebuf[i - start] = name[i];
            }
            namebuf[end - start] = 0;
            uint64_t inodenum = finditemindirectory(cur, namebuf);
            if(inodenum == -1){
                return 0;
            }
            struct file* fptr = getfileptrfromnum(inodenum);
            if(fptr == 0){
                fptr = allocfile();
                if(fptr == 0){
                    while(cur){
                        cur->ref--;
                        struct file* parent = cur->parent;
                        if(cur->ref == 0){
                            freefile(cur);
                        }
                        cur = parent;
                    }
                }
                fptr->inode = getinode(inodenum);
                assert(fptr->inode.type == file_directory);
                fptr->inodenum = inodenum;
                fptr->ref = 0;
                fptr->parent = cur;
            }
            fptr->ref++;
            assert(fptr->parent == cur);
            cur = fptr;
            start = end + 1;
        }
        end++;
    }
    return cur;
}
int64_t createfileindir(struct file* n, char* name, uint64_t type){
    assert(strlen(name) <= 12);
    if(n->inode.type != file_directory){
        return -1;
    }
    uint64_t inodenum = allocinode(type);
    if(n->inode.size == 14 * 512){
        freeinode(inodenum);
        return -1;
    }
    if(n->inode.size % 512 == 0){
        int64_t pageno = allocblock();
        if(pageno < 0){
            freeinode(inodenum);
            return -1;
        }
        uint64_t newindex = n->inode.size / 512;
        n->inode.addr[newindex] = pageno;
    }
    uint64_t childnum = n->inode.size / 16;
    uint64_t blockno = childnum / 32;
    uint64_t blockoff = childnum % 32;
    n->inode.size += 16;
    setinode(n->inodenum, &(n->inode));
    char buf[512];
    getblock(n->inode.addr[blockno], buf);
    struct dirent* d = ((struct dirent*)buf) + blockoff;
    d->inodenum = inodenum;
    strncopy(d->name, name, 12);
    setblock(n->inode.addr[blockno], buf);
    return inodenum;
}
int64_t removefilefromdir(struct file* n, uint64_t inodenum){
    if(n->inode.type != file_directory){
        return -1;
    }
    uint64_t childnum = n->inode.size / 16;
    for(uint64_t i = 0; i < 14; i++){
        uint32_t blockaddr = n->inode.addr[i];
        char block[512];
        getblock(blockaddr, block);
        for(uint64_t j = 0; j < 32; j++){
            struct dirent* d = (struct dirent*)block + j;
            if(d->inodenum == inodenum){
                uint64_t lastpage = (childnum - 1) / 32;
                char lastblock[512];
                getblock(n->inode.addr[lastpage], lastblock);
                struct dirent* lastd = (struct dirent*)lastblock + ((childnum - 1) % 32);
                *d = *lastd;
                setblock(blockaddr, block);
                if(childnum % 32 == 1){
                    freeblock(n->inode.addr[lastpage]);
                }
                n->inode.size -= 16;
                setinode(n->inodenum, &(n->inode));
                freeinode(inodenum);
                return 0;
            }
        }
    }
    return -1;
}
void truncfile(uint64_t inodenum){
    struct inode node = getinode(inodenum);
    uint64_t blocknum = (node.size + 511) / 512;
    if(blocknum <= 13){
        for(uint64_t i = 0; i < blocknum; i++){
            freeblock(node.addr[i]);
        }
    }else{
        for(uint64_t i = 0; i < 13; i++){
            freeblock(node.addr[i]);
        }
        char buf[512];
        getblock(node.addr[13], buf);
        uint32_t* c = (uint32_t*)buf;
        for(uint64_t i = 13; i < blocknum; i++){
            freeblock(c[i - 13]);
        }
        freeblock(node.addr[13]);
    }
    node.size = 0;
    setinode(inodenum, &node);
}
int64_t fileopen(char* name, uint64_t flags){
    int64_t ret = -1;
    uint64_t len = strlen(name);
    if(name[0] != '/' || len == 0 || name[len - 1] == '/'){
        return -1;
    }
    acquire(&fslock);
    uint64_t flag_read = flags & 1;
    uint64_t flag_write = (flags & 2) >> 1;
    uint64_t flag_trunc = (flags & 4) >> 2;
    if(flag_write == 0){
        flag_trunc = 0;
    }
    if(flag_read == 0 && flag_write == 0){
        release(&fslock);
        return -1;
    }
    for(uint64_t i = 0; i < 16; i++){
        if(cpu->thread->proc->pfdtable[i] == 0){
            ret = i;
            break;
        }
    }
    if(ret < 0){
        release(&fslock);
        return -1;
    }
    struct filedescriptor* fd = allocfiledescriptor();
    if(fd == 0){
        release(&fslock);
        return -1;
    }
    struct file* fn = getparentinode(name);
    if(fn == 0){
        freefiledescriptor(fd);
        release(&fslock);
        return -1;
    } 
    char namebuf[20];
    uint64_t namestart = 0;
    uint64_t ptr = 0;
    while(name[ptr] != 0){
        if(name[ptr] == '/'){
            namestart = ptr + 1;
        }
        ptr++;
    }
    for(ptr = namestart; name[ptr] != 0; ptr++){
        namebuf[ptr - namestart] = name[ptr];
    }
    namebuf[ptr - namestart] = 0;
    int64_t inodenum = finditemindirectory(fn, namebuf);
    if(inodenum < 0 && flag_write){
        inodenum = createfileindir(fn, namebuf, file_file);
    }
    if(inodenum < 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        freefiledescriptor(fd);
        release(&fslock);
        return -1;
    }
    struct file* f = getfileptrfromnum(inodenum);
    if(f == 0){
        f = allocfile();
        if(f == 0){
            while(fn != 0){
                fn->ref--;
                struct file* pa = fn->parent;
                if(fn->ref == 0){
                    freefile(fn);
                }
                fn = pa;
            }
            freefiledescriptor(fd);
            release(&fslock);
            return -1;
        }
        f->inode = getinode(inodenum);
        f->ref = 0;
        f->parent = fn;
        f->inodenum = inodenum;
    }
    f->ref++;
    assert(f->parent == fn);
    fd->fnode = f;
    fd->off = 0;
    fd->readable = (flag_read ? 1: 0);
    fd->ref = 1;
    fd->writable = (flag_write ? 1: 0);
    fd->isconsole = 0;
    cpu->thread->proc->pfdtable[ret] = fd;
    if(flag_trunc){
        truncfile(inodenum);
        f->inode = getinode(f->inodenum);
    }
    release(&fslock);
    return ret;
}
int64_t fileclose(uint64_t fdn){
    acquire(&fslock);
    struct filedescriptor* fd = cpu->thread->proc->pfdtable[fdn];
    if(fd == 0){
        release(&fslock);
        return -1;
    }
    struct file* file = fd->fnode;
    fd->ref--;
    if(fd->ref == 0){
        freefiledescriptor(fd);
    }
    while(file != 0){
        file->ref--;
        struct file* parent = file->parent;
        if(file->ref == 0){
            freefile(file);
        }
        file = parent;
    }
    cpu->thread->proc->pfdtable[fdn] = 0;
    release(&fslock);
    return 0;
}
void readblock(uint64_t blockno, uint64_t off, uint64_t size, char* buf){
    char block[512];
    getblock(blockno, block);
    for(uint64_t i = 0; i < size; i++){
        buf[i] = block[off + i];
    }
}
void writeblock(uint64_t blockno, uint64_t off, uint64_t size, char* buf){
    char block[512];
    getblock(blockno, block);
    for(uint64_t i = 0; i < size; i++){
        block[off + i] = buf[i];
    }
    setblock(blockno, block);
}
uint64_t getblockptr(struct file* f, uint64_t no){
    if(no < 13){
        return f->inode.addr[no];
    }else{
        char block[512];
        getblock(f->inode.addr[13], block);
        return *(((uint32_t*)block) + (no - 13));
    }
}
int64_t addblock(struct file* f){
    assert(f->inode.size % 512 == 0);
    if(f->inode.size / 512 == 13){
        int64_t ptrblock = allocblock();
        if(ptrblock < 0){
            return -1;
        }
        int64_t block = allocblock();
        if(block < 0){
            freeblock(block);
            return -1;
        }
        f->inode.addr[13] = ptrblock;
        setinode(f->inodenum, &(f->inode));
        char buf[512];
        getblock(ptrblock, buf);
        uint32_t* c = (uint32_t*)buf;
        c[0] = (uint32_t)block;
        setblock(ptrblock, buf);
    }else if(f->inode.size / 512 < 13){
        int64_t block = allocblock();
        if(block < 0){
            return -1;
        }
        f->inode.addr[f->inode.size / 512] = block;
        setinode(f->inodenum, &(f->inode));
    }else if(f->inode.size / 512 < 13 + 128){
        int64_t block = allocblock();
        if(block < 0){
            return -1;
        }
        char buf[512];
        getblock(f->inode.addr[13], buf);
        uint32_t* c = (uint32_t*)buf;
        c[f->inode.size / 512 - 13] = block;
        setblock(f->inode.addr[13], buf);
    }
    return 0;
}
int64_t fileread(uint64_t fdn, char* buf, uint64_t size){
    int64_t ret = 0;
    acquire(&fslock);
    struct filedescriptor* fd = cpu->thread->proc->pfdtable[fdn];
    if(fd == 0){
        release(&fslock);
        return -1;
    }
    if(!(fd->readable)){
        release(&fslock);
        return -1;
    }
    struct file* f = fd->fnode;
    uint64_t readend = (fd->off + size > fd->fnode->inode.size) ? fd->fnode->inode.size : fd->off + size;
    while(fd->off < readend){
        uint64_t block = fd->off / 512;
        uint64_t blockoff = fd->off - block * 512;
        uint64_t bsize = (size > 512 - blockoff) ? 512 - blockoff : size;
        readblock(getblockptr(f, block), blockoff, bsize, buf);
        size -= bsize;
        buf += bsize;
        fd->off += bsize;
        ret += bsize;
    }
    release(&fslock);
    return ret;
}
int64_t filewrite(uint64_t fdn, char* buf, uint64_t size){
    int64_t ret = 0;
    acquire(&fslock);
    struct filedescriptor* fd = cpu->thread->proc->pfdtable[fdn];
    if(fd == 0){
        release(&fslock);
        return -1;
    }
    if(!(fd->writable) || fd->off > fd->fnode->inode.size){
        release(&fslock);
        return -1;
    }
    struct file* f = fd->fnode;
    uint64_t writend = fd->off + size;
    while(fd->off < writend){
        uint64_t block = fd->off / 512;
        uint64_t blockoff = fd->off - block * 512;
        uint64_t bsize = (size > 512 - blockoff) ? 512 - blockoff : size;
        if(fd->off % 512 == 0 && fd->off == fd->fnode->inode.size){
            int64_t ret = addblock(f);
            if(ret < 0){
                printf("block not enough\n");
                release(&fslock);
                return ret;
            }
        }
        writeblock(getblockptr(f, block), blockoff, bsize, buf);
        size -= bsize;
        buf += bsize;
        fd->off += bsize;
        ret += bsize;
        if(fd->off > fd->fnode->inode.size){
            fd->fnode->inode.size = fd->off;
            setinode(fd->fnode->inodenum, &(fd->fnode->inode));
        }
    }
    printf("write end size = %d\n", fd->fnode->inode.size);
    release(&fslock);
    return ret;
}
int64_t fileunlink(char* name){
    uint64_t len = strlen(name);
    if(name[0] != '/' || len == 0 || name[len - 1] == '/'){
        return -1;
    }
    acquire(&fslock);
    struct file* fn = getparentinode(name);
    if(fn == 0){
        release(&fslock);
        return -1;
    }
    char namebuf[20];
    uint64_t namestart = 0;
    uint64_t ptr = 0;
    while(name[ptr] != 0){
        if(name[ptr] == '/'){
            namestart = ptr + 1;
        }
        ptr++;
    }
    for(ptr = namestart; name[ptr] != 0; ptr++){
        namebuf[ptr - namestart] = name[ptr];
    }
    namebuf[ptr - namestart] = 0;
    int64_t inodenum = finditemindirectory(fn, namebuf);
    if(inodenum < 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    struct file* f = getfileptrfromnum(inodenum);
    if(f != 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    truncfile(inodenum);
    removefilefromdir(fn, inodenum);
    while(fn != 0){
        fn->ref--;
        struct file* pa = fn->parent;
        if(fn->ref == 0){
            freefile(fn);
        }
        fn = pa;
    }
    release(&fslock);
    return 0;
}
int64_t filereaddir(char* name, struct dircontent* buf){
    uint64_t len = strlen(name);
    if(name[0] != '/' || len == 0 || name[len - 1] != '/'){
        return -1;
    }
    acquire(&fslock);
    struct file* fn = getparentinode(name);
    if(fn == 0){
        release(&fslock);
        return -1;
    }
    uint64_t childnum = fn->inode.size / 16;
    uint64_t childptr = 0;
    for(uint64_t i = 0; i < 14; i++){
        uint32_t blockaddr = fn->inode.addr[i];
        char block[512];
        getblock(blockaddr, block);
        for(uint64_t j = 0; j < 32; j++){
            struct dirent* d = (struct dirent*)block + j;
            strncopy(buf->c[childptr], d->name, 12);
            buf->c[childptr][12] = 0;
            childptr++;
            if(childptr >= childnum && childptr < 14 * 32){
                buf->c[childptr][0] = 0;
                while(fn != 0){
                    fn->ref--;
                    struct file* pa = fn->parent;
                    if(fn->ref == 0){
                        freefile(fn);
                    }
                    fn = pa;
                }
                release(&fslock);
                return 0;
            }
        }
    }
    while(fn != 0){
        fn->ref--;
        struct file* pa = fn->parent;
        if(fn->ref == 0){
            freefile(fn);
        }
        fn = pa;
    }
    release(&fslock);
    return 0;
}
int64_t filestat(char* name, struct stat* buf){
    uint64_t len = strlen(name);
    if(name[0] != '/' || len == 0){
        return -1;
    }
    acquire(&fslock);
    struct file* fn = getparentinode(name);
    if(fn == 0){
        release(&fslock);
        return -1;
    }
    if(name[len - 1] == '/'){
        buf->size = fn->inode.size;
        buf->type = fn->inode.type;
    }else{
        char namebuf[20];
        uint64_t namestart = 0;
        uint64_t ptr = 0;
        while(name[ptr] != 0){
            if(name[ptr] == '/'){
                namestart = ptr + 1;
            }
            ptr++;
        }
        for(ptr = namestart; name[ptr] != 0; ptr++){
            namebuf[ptr - namestart] = name[ptr];
        }
        namebuf[ptr - namestart] = 0;
        int64_t inodenum = finditemindirectory(fn, namebuf);
        if(inodenum < 0){
            while(fn != 0){
                fn->ref--;
                struct file* pa = fn->parent;
                if(fn->ref == 0){
                    freefile(fn);
                }
                fn = pa;
            }
            release(&fslock);
            return -1;
        }
        struct inode c = getinode(inodenum);
        buf->size = c.size;
        buf->type = c.type;
    }
    while(fn != 0){
        fn->ref--;
        struct file* pa = fn->parent;
        if(fn->ref == 0){
            freefile(fn);
        }
        fn = pa;
    }
    release(&fslock);
    return 0;
}
int64_t filemkdir(char* name){
    if(strlen(name) >= 100){
        return -1;
    }
    char cnamebuf[128];
    strncopy(cnamebuf, name, 101);
    uint64_t len = strlen(cnamebuf);
    if(cnamebuf[0] != '/' || len == 0 || cnamebuf[len - 1] != '/'){
        return -1;
    }
    cnamebuf[len - 1] = 0;
    acquire(&fslock);
    struct file* fn = getparentinode(cnamebuf);
    if(fn == 0){
        release(&fslock);
        return -1;
    }
    char namebuf[20];
    uint64_t namestart = 0;
    uint64_t ptr = 0;
    while(cnamebuf[ptr] != 0){
        if(cnamebuf[ptr] == '/'){
            namestart = ptr + 1;
        }
        ptr++;
    }
    for(ptr = namestart; cnamebuf[ptr] != 0; ptr++){
        namebuf[ptr - namestart] = cnamebuf[ptr];
    }
    namebuf[ptr - namestart] = 0;
    int64_t inodenum = finditemindirectory(fn, namebuf);
    if(inodenum >= 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    inodenum = createfileindir(fn, namebuf, file_directory);
    while(fn != 0){
        fn->ref--;
        struct file* pa = fn->parent;
        if(fn->ref == 0){
            freefile(fn);
        }
        fn = pa;
    }
    release(&fslock);
    return 0;
}
int64_t filermdir(char* name){
    if(strlen(name) >= 100){
        return -1;
    }
    char cnamebuf[128];
    strncopy(cnamebuf, name, 101);
    uint64_t len = strlen(cnamebuf);
    if(cnamebuf[0] != '/' || len <= 1 || cnamebuf[len - 1] != '/'){
        return -1;
    }
    cnamebuf[len - 1] = 0;
    acquire(&fslock);
    struct file* fn = getparentinode(cnamebuf);
    if(fn == 0){
        release(&fslock);
        return -1;
    }
    char namebuf[20];
    uint64_t namestart = 0;
    uint64_t ptr = 0;
    while(cnamebuf[ptr] != 0){
        if(cnamebuf[ptr] == '/'){
            namestart = ptr + 1;
        }
        ptr++;
    }
    for(ptr = namestart; cnamebuf[ptr] != 0; ptr++){
        namebuf[ptr - namestart] = cnamebuf[ptr];
    }
    namebuf[ptr - namestart] = 0;
    int64_t inodenum = finditemindirectory(fn, namebuf);
    if(inodenum < 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    struct file* f = getfileptrfromnum(inodenum);
    if(f != 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    struct inode node = getinode(inodenum);
    if(node.size != 0){
        while(fn != 0){
            fn->ref--;
            struct file* pa = fn->parent;
            if(fn->ref == 0){
                freefile(fn);
            }
            fn = pa;
        }
        release(&fslock);
        return -1;
    }
    removefilefromdir(fn, inodenum);
    while(fn != 0){
        fn->ref--;
        struct file* pa = fn->parent;
        if(fn->ref == 0){
            freefile(fn);
        }
        fn = pa;
    }
    release(&fslock);
    return 0;
}
int64_t fileseek(uint64_t fdn, int64_t off, uint64_t base){
    acquire(&fslock);
    struct filedescriptor* fd = cpu->thread->proc->pfdtable[fdn];
    if(fd == 0){
        release(&fslock);
        return -1;
    }
    uint64_t size = fd->fnode->inode.size;
    int64_t kbase = fd->off;
    if(base == 0){
        kbase = 0;
    }
    if(base == 2){
        kbase = size;
    }
    int64_t newoff = off + kbase;
    if(newoff < 0){
        newoff = 0;
    }
    if(newoff > size){
        newoff = size;
    }
    fd->off = newoff;
    release(&fslock);
    return fd->off;
}
void fsinit(){
    sb = readsuperblock();
    for(uint64_t i = 0; i < 512; i++){
        ftable[i].inode.type = file_unused;
    }
    for(uint64_t i = 0; i < 512; i++){
        fdtable[i].used = 0;
    }
    rootfile = allocfile();
    rootfile->ref++;
    rootfile->inode = getinode(sb.rootinode);
    assert(rootfile->inode.type == file_directory);
    rootfile->inodenum = sb.rootinode;
    rootfile->parent = 0;
    bitmappage = (uint64_t*)alloc();
    for(uint64_t i = 0; i < sb.bitmapblock; i++){
        bitmappage[i] = alloc();
        readsect(bitmappage[i], 1 + i);
    }
}