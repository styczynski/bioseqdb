package generator;

import java.util.ArrayList;
import java.util.List;
import assembly.Fragment;

public class FragmentFilter
{
	private SequenceFilter f;

	public FragmentFilter(SequenceFilter filter)
	{
		f = filter;
	}

	/**
	 * TODO: Fix this
	 *
	 * @param fragments List of fragments to generate errors in
	 * @return Errored list
	 */
	public List<? extends Fragment> generateErrors(List<? extends Fragment> fragments)
	{
		List<Fragment> list = new ArrayList<Fragment>(fragments.size());
		for (Fragment fragment : fragments)
		{
			Fragment filtered = new Fragment(f.filter(fragment.getSequence()));
			list.add(filtered);
		}
		return list;
	}
}
