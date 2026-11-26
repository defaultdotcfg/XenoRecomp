"""shared definitions (kept out of lift_t2 so that running it as a script does not duplicate the classes)"""
REGN = {'r%d' % i: i for i in range(13)}
REGN.update({'sp': 13, 'lr': 14, 'pc': 15, 'sb': 9, 'sl': 10, 'fp': 11, 'ip': 12})


class Unsupported(Exception):
    pass
