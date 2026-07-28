
"""
  Utilities to allow better control over Python Click CLI interface

  @Piotr Styczyński 2023 <piotr@styczynski.in>
  MIT LICENSE
  Algorithms for genomic data analysis | AADG | MIM UW | Bioinformatyka
"""
import click
import inspect as _inspect

from click import *
from click.formatting import join_options as _join_options

DEFAULT_CONTEXT_SETTINGS = dict(help_option_names=('-h', '--help'))

def _update_ctx_settings(context_settings):
    rv = DEFAULT_CONTEXT_SETTINGS.copy()
    if not context_settings:
        return rv
    rv.update(context_settings)
    return rv


class Command(click.Command):
    """
    :param options_metavar: The options metavar to display in the usage.
                            Defaults to ``[OPTIONS]``.
    :param args_before_options: Whether or not to display the options
                                        metavar before the arguments.
                                        Defaults to False.
    """
    def __init__(self, name, context_settings=None, callback=None, params=None,
                 help=None, epilog=None, short_help=None, add_help_option=True,
                 options_metavar='[OPTIONS]', args_before_options=True):
        super().__init__(
            name, callback=callback, params=params, help=help, epilog=epilog,
            short_help=short_help, add_help_option=add_help_option,
            context_settings=_update_ctx_settings(context_settings),
            options_metavar=options_metavar)
        self.args_before_options = args_before_options

    # overridden to support displaying args before the options metavar
    def collect_usage_pieces(self, ctx):
        rv = [] if self.args_before_options else [self.options_metavar]
        for param in self.get_params(ctx):
            rv.extend(param.get_usage_pieces(ctx))
        if self.args_before_options:
            rv.append(self.options_metavar)
        return rv

    # overridden to group arguments separately from options
    def format_options(self, ctx, formatter):
        args = []
        opts = []
        for param in self.get_params(ctx):
            rv = param.get_help_record(ctx)
            if rv is not None:
                if isinstance(param, click.Argument):
                    args.append(rv)
                else:
                    opts.append(rv)

        def print_args():
            if args:
                with formatter.section('Arguments'):
                    formatter.write_dl(args)

        def print_opts():
            if opts:
                with formatter.section(self.options_metavar):
                    formatter.write_dl(opts)

        if self.args_before_options:
            print_args()
            print_opts()
        else:
            print_opts()
            print_args()


# overridden to make sure our custom classes propagate recursively in trees of commands
class Group(click.Group):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, context_settings=_update_ctx_settings(
            kwargs.pop('context_settings', None)), **kwargs)

    def command(self, *args, **kwargs):
        return super().command(
            *args, cls=kwargs.pop('cls', Command) or Command, **kwargs)

    def group(self, *args, **kwargs):
        return super().group(
            *args, cls=kwargs.pop('cls', Group) or Group, **kwargs)


class Argument(click.Argument):
    """
    :param help: the help string.
    :param hidden: hide this option from help outputs.
                   Default is True, unless ``help`` is given.
    """
    def __init__(self, param_decls, required=None, help=None, hidden=None, **attrs):
        super().__init__(param_decls, required=required, **attrs)
        self.help = help
        self.hidden = hidden if hidden is not None else not help

    # overridden to customize the automatic formatting of metavars
    # for example, given self.name = 'query':
    # upstream | (optional) | this-method | (optional)
    # default behavior:
    # QUERY    | [QUERY]    | <query>     | [<query>]
    # when nargs > 1:
    # QUERY... | [QUERY...] | <query>, ... | [<query>, ...]
    def make_metavar(self):
        if self.metavar is not None:
            return self.metavar
        var = '' if self.required else '['
        var += '<' + self.name + '>'
        if self.nargs != 1:
            var += ', ...'
        if not self.required:
            var += ']'
        return var

    # this code is 90% copied from click.Option.get_help_record
    def get_help_record(self, ctx):
        if self.hidden:
            return

        any_prefix_is_slash = []

        def _write_opts(opts):
            rv, any_slashes = _join_options(opts)
            if any_slashes:
                any_prefix_is_slash[:] = [True]
            rv += ': ' + self.make_metavar()
            return rv

        rv = [_write_opts(self.opts)]
        if self.secondary_opts:
            rv.append(_write_opts(self.secondary_opts))

        help = self.help or ''
        extra = []

        if self.default is not None:
            if isinstance(self.default, (list, tuple)):
                default_string = ', '.join('%s' % d for d in self.default)
            elif _inspect.isfunction(self.default):
                default_string = "(dynamic)"
            else:
                default_string = self.default
            extra.append('default: {}'.format(default_string))

        if self.required:
            extra.append('required')
        if extra:
            help = '%s[%s]' % (help and help + '  ' or '', '; '.join(extra))

        return ((any_prefix_is_slash and '; ' or ' / ').join(rv), help)


def command(name=None, cls=None, **attrs):
    return click.command(name=name, cls=cls or Command, **attrs)


def group(name=None, cls=None, **attrs):
    return click.group(name=name, cls=cls or Group, **attrs)


def argument(*param_decls, cls=None, **attrs):
    return click.argument(*param_decls, cls=cls or Argument, **attrs)