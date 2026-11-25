/*
 * Lists or sets the sound cards' mixer controls. The image has no amixer, and
 * its /usr/local/bin/mixer is an unrelated program of Amazon's.
 *
 *   sndctl                         every control of every card, with its values
 *   sndctl <card> <numid> <value>  set all channels of one control
 *
 * Build: docker/build.sh make -C satellite tools
 */
#include <alsa/asoundlib.h>
#include <stdio.h>
#include <stdlib.h>

static void show(snd_ctl_t *ctl, int card, snd_ctl_elem_id_t *id)
{
    snd_ctl_elem_info_t *info;
    snd_ctl_elem_value_t *val;
    snd_ctl_elem_info_alloca(&info);
    snd_ctl_elem_value_alloca(&val);
    snd_ctl_elem_info_set_id(info, id);
    if (snd_ctl_elem_info(ctl, info) < 0)
        return;
    snd_ctl_elem_type_t type = snd_ctl_elem_info_get_type(info);
    unsigned count = snd_ctl_elem_info_get_count(info);
    printf("card %d numid %-3u %-44s", card, snd_ctl_elem_id_get_numid(id), snd_ctl_elem_id_get_name(id));
    snd_ctl_elem_value_set_id(val, id);
    if (!snd_ctl_elem_info_is_readable(info) || snd_ctl_elem_read(ctl, val) < 0) {
        printf(" (not readable)\n");
        return;
    }
    for (unsigned i = 0; i < count && i < 16; i++) {
        switch (type) {
        case SND_CTL_ELEM_TYPE_INTEGER:
            printf(" %ld", snd_ctl_elem_value_get_integer(val, i));
            break;
        case SND_CTL_ELEM_TYPE_BOOLEAN:
            printf(" %s", snd_ctl_elem_value_get_boolean(val, i) ? "on" : "off");
            break;
        case SND_CTL_ELEM_TYPE_ENUMERATED: {
            unsigned item = snd_ctl_elem_value_get_enumerated(val, i);
            snd_ctl_elem_info_set_item(info, item);
            if (snd_ctl_elem_info(ctl, info) == 0)
                printf(" '%s'", snd_ctl_elem_info_get_item_name(info));
            else
                printf(" #%u", item);
            break;
        }
        default:
            printf(" ?");
        }
    }
    if (type == SND_CTL_ELEM_TYPE_INTEGER)
        printf("   (range %ld..%ld)", snd_ctl_elem_info_get_min(info), snd_ctl_elem_info_get_max(info));
    printf("\n");
}

int main(int argc, char **argv)
{
    int set = argc == 4;
    if (argc != 1 && !set) {
        fprintf(stderr, "usage: %s [<card> <numid> <value>]\n", argv[0]);
        return 2;
    }
    int found = 0;
    for (int card = 0; card < 8; card++) {
        char name[16];
        snd_ctl_t *ctl;
        if (set && card != atoi(argv[1]))
            continue;
        snprintf(name, sizeof name, "hw:%d", card);
        if (snd_ctl_open(&ctl, name, 0) < 0)
            continue;
        found = 1;
        snd_ctl_elem_list_t *list;
        snd_ctl_elem_list_alloca(&list);
        snd_ctl_elem_list(ctl, list);
        unsigned n = snd_ctl_elem_list_get_count(list);
        snd_ctl_elem_list_alloc_space(list, n);
        snd_ctl_elem_list(ctl, list);
        for (unsigned i = 0; i < n; i++) {
            snd_ctl_elem_id_t *id;
            snd_ctl_elem_id_alloca(&id);
            snd_ctl_elem_list_get_id(list, i, id);
            if (set) {
                if (snd_ctl_elem_id_get_numid(id) != (unsigned)atoi(argv[2]))
                    continue;
                snd_ctl_elem_info_t *info;
                snd_ctl_elem_value_t *val;
                snd_ctl_elem_info_alloca(&info);
                snd_ctl_elem_value_alloca(&val);
                snd_ctl_elem_info_set_id(info, id);
                snd_ctl_elem_info(ctl, info);
                snd_ctl_elem_value_set_id(val, id);
                snd_ctl_elem_read(ctl, val);
                printf("before: ");
                show(ctl, card, id);
                for (unsigned c = 0; c < snd_ctl_elem_info_get_count(info); c++) {
                    if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_INTEGER)
                        snd_ctl_elem_value_set_integer(val, c, atol(argv[3]));
                    else if (snd_ctl_elem_info_get_type(info) == SND_CTL_ELEM_TYPE_BOOLEAN)
                        snd_ctl_elem_value_set_boolean(val, c, atol(argv[3]));
                    else
                        snd_ctl_elem_value_set_enumerated(val, c, (unsigned)atol(argv[3]));
                }
                int r = snd_ctl_elem_write(ctl, val);
                if (r < 0)
                    printf("write failed: %s\n", snd_strerror(r));
                printf("after:  ");
            }
            show(ctl, card, id);
        }
        snd_ctl_elem_list_free_space(list);
        snd_ctl_close(ctl);
    }
    if (!found)
        fprintf(stderr, "no sound card\n");
    return !found;
}
